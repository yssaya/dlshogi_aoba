﻿#include "init.hpp"
#include "position.hpp"
#include "usi.hpp"
#include "move.hpp"
#include "generateMoves.hpp"
#include "search.hpp"
#include "book.hpp"

#include "cppshogi.h"
#include "UctSearch.h"
#include "Message.h"
#include "dfpn.h"
#include "aobabook.h"

#include <future>
#ifdef MAKE_BOOK
#include <sys/stat.h>
#endif


#ifdef MULTI_PONDER
#include "../usi_multiponder/USIPonderEngine.h"
std::atomic<bool> need_multi_ponder = false;
constexpr size_t nohit = (size_t)-1;
#endif

extern std::ostream& operator << (std::ostream& os, const OptionsMap& om);

struct MySearcher : Searcher {
	static void doUSICommandLoop(int argc, char* argv[]);
#ifdef MAKE_BOOK
	static void makeBook(std::istringstream& ssCmd);
#endif
	static Key starting_pos_key;
	static std::vector<Move> moves;
	static std::promise<std::pair<Move, Move>> promise;
	static std::future<std::pair<Move, Move>> future;
	static void setPositionAndLimits(Position& pos, std::istringstream& ssCmd, const std::string& posCmd);
	static void goUct(Position& pos);
#ifndef MULTI_PONDER
	static void getAndPrintBestMove();
#else
	static std::pair<Move, Move> getAndPrintBestMove();
#endif
};

Key MySearcher::starting_pos_key;
std::vector<Move> MySearcher::moves;
std::promise<std::pair<Move, Move>> MySearcher::promise;
std::future<std::pair<Move, Move>> MySearcher::future;

DfPn dfpn;
int dfpn_min_search_millisecs = 300;

#if defined(MAKE_BOOK) || defined(BOOK_POLICY)
std::map<Key, std::vector<BookEntry> > bookMap;
bool use_book_policy = false;
bool use_book_value = false;

// 定跡読み込み
void read_book(const std::string& bookFileName, std::map<Key, std::vector<BookEntry> >& bookMap) {
	std::ifstream ifs(bookFileName.c_str(), std::ifstream::in | std::ifstream::binary);
	if (!ifs) {
		std::cerr << "Error: cannot open " << bookFileName << std::endl;
		exit(EXIT_FAILURE);
	}
	BookEntry entry;
	size_t count = 0;
	while (ifs.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
		count++;
		auto itr = bookMap.find(entry.key);
		if (itr != bookMap.end()) {
			// すでにある場合、追加
			itr->second.emplace_back(entry);
		}
		else {
			bookMap[entry.key].emplace_back(entry);
		}
	}
	std::cout << "bookEntries.size:" << bookMap.size() << " count:" << count << std::endl;
}
#endif

int main(int argc, char* argv[]) {
	initTable();
	Position::initZobrist();
	//HuffmanCodedPos::init();
	auto s = std::unique_ptr<MySearcher>(new MySearcher);

	s->init();
	s->doUSICommandLoop(argc, argv);

	// リソースの破棄はOSに任せてすぐに終了する
	std::quick_exit(0);
}

#ifdef AOBA_BOOK
std::string latestPosCmd;
#endif

void MySearcher::doUSICommandLoop(int argc, char* argv[]) {
	bool evalTableIsRead = false;
	Position pos(DefaultStartPositionSFEN, thisptr);
	std::string posCmd("startpos");

	std::string cmd;
	std::string token;
	std::thread th;

#ifdef MULTI_PONDER
	std::vector<Move> multi_ponder_moves;
	std::vector<size_t> multi_ponder_engine_index;
	std::vector<USIPonderEngine> usi_ponder_engines;
#endif

	for (int i = 1; i < argc; ++i)
		cmd += std::string(argv[i]) + " ";

	do {
		if (argc == 1 && !std::getline(std::cin, cmd))
			cmd = "quit";

		std::istringstream ssCmd(cmd);
		token.clear();

		ssCmd >> std::skipws >> token;

		if (token == "quit") {
#ifdef MULTI_PONDER
			for (size_t i = 0; i < usi_ponder_engines.size(); i++) {
				usi_ponder_engines[i].Quit();
			}
#endif
			StopUctSearch();
			TerminateUctSearch();
			if (th.joinable())
				th.join();
			FinalizeUctSearch();
		}
		else if (token == "gameover") {
			StopUctSearch();
			if (th.joinable())
				th.join();
			GameOver();
		}
		else if (token == "stop") {
			StopUctSearch();
			if (th.joinable())
				th.join();
			if (limits.ponder) {
				// 無視されるがbestmoveを返す
				std::cout << "bestmove resign" << std::endl;
			}
		}
		else if (token == "go") {
#ifdef MULTI_PONDER
			USIPonderResult ponderResult;
			size_t ponderhit_i = nohit;
			if (need_multi_ponder) {
				for (size_t i : multi_ponder_engine_index) {
					if (usi_ponder_engines[i].IsLiving()) {
						if (posCmd == usi_ponder_engines[i].GetUsiPosition())
							ponderhit_i = i;
						else
							usi_ponder_engines[i].Stop();
					}
				}
				if (ponderhit_i != nohit ) {
					std::cout << "info string multiponder " << ponderhit_i + 1 << " ponderhit" << std::endl;
					ponderResult = usi_ponder_engines[ponderhit_i].Ponderhit();
				}
				for (size_t i : multi_ponder_engine_index) {
					if (usi_ponder_engines[i].IsLiving())
						usi_ponder_engines[i].Join();
				}
			}

			ResetMultiPonder();
			need_multi_ponder = false;
			multi_ponder_moves.clear();
			multi_ponder_engine_index.clear();
#endif
			// ponderの探索を停止
			StopUctSearch();
			if (th.joinable())
				th.join();
			setPositionAndLimits(pos, ssCmd, posCmd);
			InitUctSearchStop();
			// ponderhitで探索を継続するため、bestmoveはfutureで受け取る
			promise = std::promise<std::pair<Move, Move>>();
			future = promise.get_future();
#ifdef MULTI_PONDER
			if (ponderResult.bestMove != "") {
				// multi ponderがponderhitの場合
				std::cout << ponderResult.info << "\n";
				if (ponderResult.bestMove == "resign")
					promise.set_value({ Move::moveResign(), Move::moveNone() });
				else if (ponderResult.bestMove == "win")
					promise.set_value({ Move::moveWin(), Move::moveNone() });
				else {
					// 相手局面から探索を継続する
					Move move = usiToMove(pos, ponderResult.bestMove);
					need_multi_ponder = true;
					promise.set_value({ move, Move::moveNone() });
					th = std::thread([&pos, move] {
						StateInfo st;
						pos.doMove(move, st);

						Move ponderMove;
						moves.emplace_back(move);
						SetPondering(true);
						UctSearchGenmove(&pos, starting_pos_key, moves, ponderMove);
					});
				}
			}
			else
			{
				th = std::thread([&pos] {
					goUct(pos);
				});
			}
#else
			th = std::thread([&pos] {
				goUct(pos);
			});
#endif
			if (!limits.ponder) {
#ifndef MULTI_PONDER
				getAndPrintBestMove();
#else
				const auto bestMove = getAndPrintBestMove();
				if (need_multi_ponder) {
					// ルートノードが展開されるのを待つ
					WaitPrepareMultiPonder();

					// 訪問回数・方策順にソートした指し手を取得
					const bool ret = GetMultiPonderMove(multi_ponder_moves, options["Multi_Ponder"]);

					if (ret && multi_ponder_moves.size() > 0) {
						std::string pos_ss(posCmd);
						if (posCmd.find("moves") == posCmd.npos) {
							pos_ss += " moves";
						}
						pos_ss += " " + bestMove.first.toUSI();
						// USIエンジンにgo ponderを送信
						std::cout << "info string multiponder";
						for (size_t i = 0, j = 0; i < multi_ponder_moves.size(); i++) {
							size_t ponder_i = i;
							// ponderhitしたエンジンに1番目の候補手を割り当てる
							if (ponderhit_i != nohit) {
								if (i == 0) {
									ponder_i = ponderhit_i;
								}
								else if (i <= ponderhit_i) {
									ponder_i = i - 1;
								}
							}
							multi_ponder_engine_index.emplace_back(ponder_i);
							if (usi_ponder_engines[ponder_i].IsLiving()) {
								const std::string ponder_move = std::move(multi_ponder_moves[j++].toUSI());
								std::cout << " " << ponder_i + 1 << " " << ponder_move;
								usi_ponder_engines[ponder_i].GoPonderAsync(pos_ss + " " + ponder_move, limits);
							}
						}
						std::cout << "\n";
					}
					else {
						need_multi_ponder = false;
					}
				}
#endif
			}
		}
		else if (token == "ponderhit") {
			// go ponderの探索をそのまま継続し、ponderingの状態のみ変更する
			SetPondering(false);
			getAndPrintBestMove();
		}
		else if (token == "position") {
			// 探索中にsetPositionを行うとStateInfoが壊れるため、探索を停止してから変更する必要があるため、
			// 文字列の保存のみ行う
			posCmd = cmd.substr((size_t)ssCmd.tellg() + 1);
#ifdef AOBA_BOOK
			latestPosCmd = posCmd;
#endif
		}
		else if (token == "usinewgame"); // isready で準備は出来たので、対局開始時に特にする事はない。
		else if (token == "usi") std::cout << "id name " << std::string(options["Engine_Name"])
			<< "\nid author Tadao Yamaoka"
			<< "\n" << options
			<< "\nusiok" << std::endl;
		else if (token == "isready") { // 対局開始前の準備。
			static bool initialized = false;
			if (!initialized) {
				// 各種初期化
				InitializeUctSearch(options["UCT_NodeLimit"]);
//				const std::string model_paths[max_gpu] = { options["DNN_Model"], options["DNN_Model2"], options["DNN_Model3"], options["DNN_Model4"], options["DNN_Model5"], options["DNN_Model6"], options["DNN_Model7"], options["DNN_Model8"] };
				const std::string model_paths[max_gpu] = { options["DNN_Model"], options["DNN_Model2"], options["DNN_Model3"], options["DNN_Model4"], options["DNN_Model5"], options["DNN_Model6"], options["DNN_Model7"], options["DNN_Model8"], options["DNN_Model9"], options["DNN_Model10"], options["DNN_Model11"], options["DNN_Model12"], options["DNN_Model13"], options["DNN_Model14"], options["DNN_Model15"], options["DNN_Model16"], options["DNN_Model17"], options["DNN_Model18"] };
				// モデルファイル存在チェック
				{
					bool is_err = false;
					for (int i = 0; i < max_gpu; ++i) {
						if (model_paths[i] != "") {
							std::ifstream ifs(model_paths[i].c_str());
							if (!ifs.is_open()) {
								std::cout << model_paths[i] << " file not found" << std::endl;
								is_err = true;
								break;
							}
						}
					}
					if (is_err)
						break;
				}
				SetModelPath(model_paths);
				// モデルの.iniファイルにしたがってデフォルトパラメータ変更
				std::ifstream is = std::ifstream(model_paths[0] + ".ini");
				while (is) {
					std::string line;
					is >> line;
					if (line != "") {
						const auto pos = line.find_first_of('=');
						const auto name = line.substr(0, pos);
						if (options[name].isDefault()) {
							options[name] = line.substr(pos + 1);
							std::cout << "info string " << name << "=" << options[name] << std::endl;
						}
					}
				}
#ifdef BOOK_POLICY
                // 事前確率に定跡の遷移確率も使用する
                use_book_policy = options["Use_Book_Policy"];
                use_book_value = options["Use_Book_Value"];
                if (use_book_policy || use_book_value) {
                    // 定跡読み込み
                    read_book(options["Book_File"], bookMap);
                }
#endif
//              const int new_thread[max_gpu] = { options["UCT_Threads"], options["UCT_Threads2"], options["UCT_Threads3"], options["UCT_Threads4"], options["UCT_Threads5"], options["UCT_Threads6"], options["UCT_Threads7"], options["UCT_Threads8"] };
				const int new_thread[max_gpu] = { options["UCT_Threads"], options["UCT_Threads2"], options["UCT_Threads3"], options["UCT_Threads4"], options["UCT_Threads5"], options["UCT_Threads6"], options["UCT_Threads7"], options["UCT_Threads8"], options["UCT_Threads9"], options["UCT_Threads10"], options["UCT_Threads11"], options["UCT_Threads12"], options["UCT_Threads13"], options["UCT_Threads14"], options["UCT_Threads15"], options["UCT_Threads16"], options["UCT_Threads17"], options["UCT_Threads18"] };
//				const int new_policy_value_batch_maxsize[max_gpu] = { options["DNN_Batch_Size"], options["DNN_Batch_Size2"], options["DNN_Batch_Size3"], options["DNN_Batch_Size4"], options["DNN_Batch_Size5"], options["DNN_Batch_Size6"], options["DNN_Batch_Size7"], options["DNN_Batch_Size8"] };
				const int new_policy_value_batch_maxsize[max_gpu] = { options["DNN_Batch_Size"], options["DNN_Batch_Size2"], options["DNN_Batch_Size3"], options["DNN_Batch_Size4"], options["DNN_Batch_Size5"], options["DNN_Batch_Size6"], options["DNN_Batch_Size7"], options["DNN_Batch_Size8"], options["DNN_Batch_Size9"], options["DNN_Batch_Size10"], options["DNN_Batch_Size11"], options["DNN_Batch_Size12"], options["DNN_Batch_Size13"], options["DNN_Batch_Size14"], options["DNN_Batch_Size15"], options["DNN_Batch_Size16"], options["DNN_Batch_Size17"], options["DNN_Batch_Size18"] };
				SetThread(new_thread, new_policy_value_batch_maxsize);

				if (options["Mate_Root_Search"] > 0) {
					DfPn::set_hashsize(options["DfPn_Hash"]);
					dfpn.init();
				}

#ifdef PV_MATE_SEARCH
				// PVの詰み探索の設定
				if (options["PV_Mate_Search_Threads"] > 0) {
					SetPvMateSearch(options["PV_Mate_Search_Threads"], options["PV_Mate_Search_Depth"], options["PV_Mate_Search_Nodes"]);
				}
#endif
#ifdef MULTI_PONDER
				// デストラクタが実行されないようにreserveする
				usi_ponder_engines.reserve(options["Multi_Ponder"]);
				for (int i = 0; i < options["Multi_Ponder"]; i++) {
					std::vector<std::pair<std::string, std::string>> usi_engine_options;
					std::string engine_options = options["Multi_Ponder_Engine" + std::to_string(i + 1) + "_Options"];
					// usiToCsa.rbでは,が使えないため#でも区切ることができるようにする
					std::replace(engine_options.begin(), engine_options.end(), '#', ',');
					std::istringstream ss(engine_options);
					std::string field;
					while (std::getline(ss, field, ',')) {
						const auto p = field.find_first_of(":");
						usi_engine_options.emplace_back(field.substr(0, p), field.substr(p + 1));
					}
					usi_ponder_engines.emplace_back((std::string)options["Multi_Ponder_Engine" + std::to_string(i + 1)], usi_engine_options);
				}
				for (size_t i = 0; i < usi_ponder_engines.size(); i++) {
					usi_ponder_engines[i].WaitInit();
				}
#endif
			}
			initialized = true;
#ifdef MULTI_PONDER
			need_multi_ponder = false;
#endif

			NewGame();

			// 詰み探索用
			if (options["Mate_Root_Search"] > 0) {
				dfpn.set_maxdepth(options["Mate_Root_Search"]);
				const int draw_ply = options["Draw_Ply"];
				if (draw_ply > 0)
					DfPn::set_draw_ply(draw_ply);
			}

			// オプション設定
			set_softmax_temperature(options["Softmax_Temperature"] / 100.0f);
			SetResignThreshold(options["Resign_Threshold"]);
			SetDrawPly(options["Draw_Ply"]);
			SetDrawValue(options["Draw_Value_Black"], options["Draw_Value_White"]);
			dfpn_min_search_millisecs = options["DfPn_Min_Search_Millisecs"];
			c_init = options["C_init"] / 100.0f;
			c_base = (float)options["C_base"];
			c_fpu_reduction = options["C_fpu_reduction"] / 100.0f;
			c_init_root = options["C_init_root"] / 100.0f;
			c_base_root = (float)options["C_base_root"];
			c_fpu_reduction_root = options["C_fpu_reduction_root"] / 100.0f;
			SetReuseSubtree(options["ReuseSubtree"]);
			SetPvInterval(options["PV_Interval"]);
			SetMultiPV(options["MultiPV"]);
			SetEvalCoef(options["Eval_Coef"]);
			SetRandomMove(options["Random_Ply"], options["Random_Temperature"], options["Random_Temperature_Drop"], options["Random_Cutoff"], options["Random_Cutoff_Drop"]);
			SetRandomMove2(options["Random2_Ply"], options["Random2_Probability"], options["Random2_Temperature"], options["Random2_Cutoff"], options["Random2_Value_Limit"]);

			// DebugMessageMode
			SetDebugMessageMode(options["DebugMessage"]);

			// 初回探索をキャッシュ
			Position pos_tmp(DefaultStartPositionSFEN, thisptr);
			LimitsType limits;
			limits.startTime.restart();
			limits.nodes = 1;

			SetLimits(limits);
			Move ponder;
			UctSearchGenmove(&pos_tmp, pos_tmp.getKey(), {}, ponder);

#ifdef AOBA_BOOK
			read_sfen_best();
#endif

			// 固定プレイアウトモード
			SetConstPlayout(options["Const_Playout"]);

			// PonderingMode
			SetPonderingMode(options["USI_Ponder"] && !options["Stochastic_Ponder"]);

			std::cout << "readyok" << std::endl;
		}
		else if (token == "setoption") {
			setOption(ssCmd);
			SetMultiPV(options["MultiPV"]);
		}
#ifdef MAKE_BOOK
		else if (token == "make_book") makeBook(ssCmd);
#endif
	} while (token != "quit" && argc == 1);

	if (th.joinable())
		th.join();
}

void MySearcher::setPositionAndLimits(Position& pos, std::istringstream& ssCmd, const std::string& posCmd) {
	std::string token;

	limits.startTime.restart();

	// 探索開始局面設定
	// 持ち時間設定よりも前に実行が必要
	moves.clear();
	{
		std::istringstream ssPosCmd(posCmd);
		std::string token;
		std::string sfen;

		ssPosCmd >> token;

		if (token == "startpos") {
			sfen = DefaultStartPositionSFEN;
			ssPosCmd >> token; // "moves" が入力されるはず。
		}
		else if (token == "sfen") {
			while (ssPosCmd >> token && token != "moves")
				sfen += token + " ";
		}
		else
			return;

		pos.set(sfen);
		states = StateListPtr(new std::deque<StateInfo>(1));

		starting_pos_key = pos.getKey();

		while (ssPosCmd >> token) {
			const Move move = usiToMove(pos, token);
			if (!move) break;
			pos.doMove(move, states->emplace_back());
			moves.emplace_back(move);
		}
	}

	// limitsをクリアして再設定
	limits.nodes = limits.time[Black] = limits.time[White] = limits.inc[Black] = limits.inc[White] = limits.movesToGo = limits.moveTime = limits.mate = limits.infinite = limits.ponder = 0;
	ssCmd >> token;
	if (token == "ponder") {
		limits.ponder = true;
	}
	do {
		if (token == "btime") ssCmd >> limits.time[Black];
		else if (token == "wtime") ssCmd >> limits.time[White];
		else if (token == "binc") ssCmd >> limits.inc[Black];
		else if (token == "winc") ssCmd >> limits.inc[White];
		else if (token == "infinite") limits.infinite = true;
		else if (token == "byoyomi" || token == "movetime") ssCmd >> limits.moveTime;
		else if (token == "mate") ssCmd >> limits.mate;
		else if (token == "nodes") ssCmd >> limits.nodes;
	} while (ssCmd >> token);
	if (limits.moveTime != 0) {
		limits.moveTime -= options["Byoyomi_Margin"];
	}
	else if (options["Time_Margin"] != 0) {
		limits.time[pos.turn()] -= options["Time_Margin"];
	}

	SetLimits(&pos, limits);
}

void MySearcher::goUct(Position& pos) {
	Move ponderMove = Move::moveNone();

#ifdef AOBA_BOOK
	{
		std::string sfen = "position " + pos.toSFEN();
		std::cerr << "aobabook sfen=" << sfen << std::endl;
		std::cerr << "latestPosCmd=" << latestPosCmd << std::endl;
		std::string bookmove = get_dlshogi_book(sfen, latestPosCmd);
		if ( bookmove.size() ) {
			auto move = usiToMove(pos, bookmove);
			std::cout << "info" << " score cp 0 pv " << bookmove << std::endl;
			promise.set_value({ move, Move::moveNone() });
			return;
		}
	}
#endif

	// Book使用
	static Book book;
	if (options["OwnBook"]) {
		const std::tuple<Move, Score> bookMoveScore = options["Book_Consider_Draw"] ?
			(options["Book_Consider_Draw_Depth"] > 0 ?
				book.probeConsideringDrawDepth(pos, options["Book_File"]) : book.probeConsideringDraw(pos, options["Book_File"]))
			: book.probe(pos, options["Book_File"], options["Best_Book_Move"]);
		if (std::get<0>(bookMoveScore)) {
			std::cout << "info"
				<< " score cp " << std::get<1>(bookMoveScore)
				<< " pv " << std::get<0>(bookMoveScore).toUSI()
				<< std::endl;

#ifdef MULTI_PONDER
			if (options["USI_Ponder"] && options["Stochastic_Ponder"])
				need_multi_ponder = true;
#endif
			auto move = std::get<0>(bookMoveScore);
			promise.set_value({ move, Move::moveNone() });

			// 確率的なPonderの場合、相手局面から探索を継続する
			if (options["USI_Ponder"] && options["Stochastic_Ponder"]) {
				StateInfo st;
				pos.doMove(move, st);

				moves.emplace_back(move);
				SetPondering(true);
				UctSearchGenmove(&pos, starting_pos_key, moves, ponderMove);
			}
			return;
		}
	}

	// 入玉勝ちかどうかを判定
	if (nyugyoku(pos)) {
		promise.set_value({ Move::moveWin(), Move::moveNone() });
		return;
	}

	// 詰みの探索用
	std::unique_ptr<std::thread> t;
	dfpn.dfpn_stop(false);
	std::atomic<bool> dfpn_done(false);
	bool mate = false;
	const uint32_t mate_depth = options["Mate_Root_Search"];
	Position pos_copy(pos);
	if (mate_depth > 0) {
		t.reset(new std::thread([&pos_copy, &mate, &dfpn_done]() {
			mate = dfpn.dfpn(pos_copy);
			if (mate)
				StopUctSearch();
			dfpn_done = true;
		}));
	}

	// UCTによる探索
	Move move = UctSearchGenmove(&pos, starting_pos_key, moves, ponderMove);

	// 詰み探索待ち
	if (mate_depth > 0) {
		// 最小詰み探索時間の間待つ
		const int time_limit = GetTimeLimit();
		while (!dfpn_done) {
			const auto elapse = limits.startTime.elapsed();
			if (elapse >= time_limit ||
				elapse >= dfpn_min_search_millisecs)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		dfpn.dfpn_stop(true);
		t->join();
		if (mate) {
			// 詰み
			std::string mate_pv;
			int mate_depth;
			Move mate_move;
			std::tie(mate_pv, mate_depth, mate_move) = dfpn.get_pv(pos);
			// PV表示
			if (mate_depth > 0)
				std::cout << "info score mate "<< mate_depth << " pv " << mate_pv;
			else
				std::cout << "info score mate + pv " << mate_pv;
			std::cout << std::endl;
			promise.set_value({ mate_move, Move::moveNone() });
			return;
		}
	}

	if (move == Move::moveNone()) {
		promise.set_value({ Move::moveResign(), Move::moveNone() });
		return;
	}
	// 確率的なPonderの場合、ponderを返さない
	if (!IsUctSearchStoped() && options["USI_Ponder"] && options["Stochastic_Ponder"]) {
#ifdef MULTI_PONDER
		need_multi_ponder = true;
		limits.time[pos.turn()] -= std::min(limits.time[pos.turn()], (limits.startTime.elapsed() + 999) / 1000 * 1000);
#endif
		promise.set_value({ move, Move::moveNone() });

		// 相手局面から探索を継続する
		StateInfo st;
		pos.doMove(move, st);

		moves.emplace_back(move);
		SetPondering(true);
		UctSearchGenmove(&pos, starting_pos_key, moves, ponderMove);
	}
	else {
		promise.set_value({ move, ponderMove });
	}
}

#ifndef MULTI_PONDER
void MySearcher::getAndPrintBestMove() {
	std::thread([] {
		const auto bestMove = future.get();
		std::cout << "bestmove ";
		if (bestMove.first == Move::moveResign()) {
			std::cout << "resign";
		}
		else if (bestMove.first == Move::moveWin()) {
			std::cout << "win";
		}
		else {
			std::cout << bestMove.first.toUSI();
		}
		if (bestMove.second != Move::moveNone()) {
			std::cout << " ponder " << bestMove.second.toUSI();
		}
		std::cout << std::endl;
	}).detach();
}
#else
std::pair<Move, Move> MySearcher::getAndPrintBestMove() {
	auto bestMove = future.get();
	std::cout << "bestmove ";
	if (bestMove.first == Move::moveResign()) {
		std::cout << "resign";
	}
	else if (bestMove.first == Move::moveWin()) {
		std::cout << "win";
	}
	else {
		std::cout << bestMove.first.toUSI();
	}
	if (bestMove.second != Move::moveNone()) {
		std::cout << " ponder " << bestMove.second.toUSI();
	}
	std::cout << std::endl;

	return bestMove;
}
#endif

#ifdef MAKE_BOOK
struct child_node_t_copy {
	Move move;       // 着手する座標
	int move_count;  // 探索回数
	WinType win;       // 勝った回数

	child_node_t_copy(const child_node_t& child) {
		this->move = child.move;
		this->move_count = child.move_count;
		this->win = child.win;
	}
};

Key book_starting_pos_key;
extern std::unique_ptr<NodeTree> tree;
int make_book_sleep = 0;
bool use_interruption = true;
int book_eval_threshold = INT_MAX;
double book_visit_threshold = 0.005;
double book_cutoff = 0.015;
double book_reciprocal_temperature = 1.0;

inline Move UctSearchGenmoveNoPonder(Position* pos, const std::vector<Move>& moves) {
	Move move;
	return UctSearchGenmove(pos, book_starting_pos_key, moves, move);
}

bool make_book_entry_with_uct(Position& pos, LimitsType& limits, const Key& key, std::map<Key, std::vector<BookEntry> > &outMap, int& count, const std::vector<Move> &moves) {
	std::cout << "position startpos moves";
	for (Move move : moves) {
		std::cout << " " << move.toUSI();
	}
	std::cout << std::endl;

	// UCT探索を使用
	limits.startTime.restart();
	SetLimits(limits);
	UctSearchGenmoveNoPonder(&pos, moves);

	const uct_node_t* current_root = tree->GetCurrentHead();
	if (current_root->child_num == 0) {
		// 詰みの局面
		return false;
	}

	// 探索回数で降順ソート
	std::vector<child_node_t_copy> movelist;
	int num = 0;
	const child_node_t *uct_child = current_root->child.get();
	for (int i = 0; i < current_root->child_num; i++) {
		movelist.emplace_back(uct_child[i]);
	}

	std::sort(movelist.begin(), movelist.end(), [](auto left, auto right) {
		return left.move_count > right.move_count;
	});

	const auto cutoff_threshold = movelist[0].win / movelist[0].move_count - book_cutoff;
	for (const auto& child : movelist) {
		if (double(child.move_count) / current_root->move_count <= book_visit_threshold) // 訪問回数閾値
			break;
		if (child.win / child.move_count < cutoff_threshold) // 勝率閾値
			break;
		num++;
	}
	if (num == 0) {
		num = (current_root->child_num + 2) / 3;
	}

	std::cout << "movelist.size: " << num << std::endl;

	for (int i = 0; i < num; i++) {
		const auto &child = movelist[i];
		// 定跡追加
		BookEntry be;
		const auto wintrate = child.win / child.move_count;
		be.score = Score(int(-log(1.0 / wintrate - 1.0) * 754.3));
		be.key = key;
		be.fromToPro = static_cast<u16>(child.move.proFromAndTo());
		be.count = (u16)((double)child.move_count / (double)current_root->move_count * 1000.0);
		outMap[key].emplace_back(be);

		count++;
	}

	if (make_book_sleep > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(make_book_sleep));

	return true;
}

// 定跡作成(再帰処理)
void make_book_inner(Position& pos, LimitsType& limits, std::map<Key, std::vector<BookEntry> >& bookMap, std::map<Key, std::vector<BookEntry> > &outMap, int& count, const int depth, const bool isBlack, std::vector<Move> &moves) {
	const Key key = Book::bookKey(pos);
	if ((depth % 2 == 0) == isBlack) {

		if (outMap.find(key) == outMap.end()) {
			// 先端ノード
			// UCT探索で定跡作成
			make_book_entry_with_uct(pos, limits, key, outMap, count, moves);
		}
		else {
			// 探索済みの場合
			{
				// 最上位の手を選択
				// (定跡の幅を広げたい場合は確率的に選択するように変更する)
				const auto entry = outMap[key][0];

				// 評価値が閾値を超えた場合、探索終了
				if (std::abs(entry.score) > book_eval_threshold) {
					std::cout << "position startpos moves";
					for (Move move : moves) {
						std::cout << " " << move.toUSI();
					}
					std::cout << "\nentry.score: " << entry.score << std::endl;
					return;
				}

				const Move move = move16toMove(Move(entry.fromToPro), pos);

				StateInfo state;
				pos.doMove(move, state);
				// 繰り返しになる場合、探索終了
				if (pos.isDraw() == RepetitionDraw) {
					pos.undoMove(move);
					return;
				}

				moves.emplace_back(move);

				// 次の手を探索
				make_book_inner(pos, limits, bookMap, outMap, count, depth + 1, isBlack, moves);

				pos.undoMove(move);
			}
		}
	}
	else {
		// 定跡を使用
		std::vector<BookEntry> *entries;

		// 局面が定跡にあるか確認
		auto itr = bookMap.find(key);
		if (itr != bookMap.end()) {
			entries = &itr->second;
		}
		else {
			// 定跡にない場合、探索結果を使う
			itr = outMap.find(Book::bookKey(pos));

			if (itr == outMap.end()) {
				// 定跡になく未探索の局面の場合
				// UCT探索で定跡作成
				if (!make_book_entry_with_uct(pos, limits, key, outMap, count, moves))
				{
					// 詰みの局面の場合何もしない
					return;
				}
			}

			entries = &outMap[key];
		}

		// 確率的に手を選択
		std::vector<double> probabilities;
		for (const auto& entry : *entries) {
			const auto probability = std::pow((double)entry.count, book_reciprocal_temperature);
			probabilities.emplace_back(probability);
		}
		std::discrete_distribution<std::size_t> dist(probabilities.begin(), probabilities.end());
		size_t selected = dist(g_randomTimeSeed);

		const Move move = move16toMove(Move(entries->at(selected).fromToPro), pos);

		StateInfo state;
		pos.doMove(move, state);
		moves.emplace_back(move);

		// 次の手を探索
		make_book_inner(pos, limits, bookMap, outMap, count, depth + 1, isBlack, moves);

		pos.undoMove(move);
	}
}

// 定跡マージ
int merge_book(std::map<Key, std::vector<BookEntry> >& outMap, const std::string& merge_file) {
	// ファイル更新がある場合のみ処理する
	static time_t prev_time = 0;
	struct stat st;
	if (stat(merge_file.c_str(), &st) != 0)
		return 0;
	if (st.st_ctime == prev_time)
		return 0;
	prev_time = st.st_ctime;

	std::ifstream ifsMerge(merge_file.c_str(), std::ios::binary);
	int merged = 0;
	if (ifsMerge) {
		BookEntry entry;
		Key prev_key = 0;
		while (ifsMerge.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
			if (entry.key == prev_key || outMap.find(entry.key) == outMap.end()) {
				if (entry.key != prev_key)
					merged++;
				outMap[entry.key].emplace_back(entry);
				prev_key = entry.key;
			}
		}
	}
	std::cout << "merged: " << merged << std::endl;
	return merged;
}

// 定跡作成
void MySearcher::makeBook(std::istringstream& ssCmd) {
	// isreadyを先に実行しておくこと。

	std::string bookFileName;
	std::string outFileName;
	int playoutNum;
	int limitTrialNum;

	ssCmd >> bookFileName;
	ssCmd >> outFileName;
	ssCmd >> playoutNum;
	ssCmd >> limitTrialNum;

	// プレイアウト数固定
	LimitsType limits;
	limits.nodes = playoutNum;

	// 保存間隔
	const int save_book_interval = options["Save_Book_Interval"];

	// 1定跡作成ごとのスリープ時間(ガベージコレクションが間に合わない場合に設定する)
	make_book_sleep = options["Make_Book_Sleep"];

	// 事前確率に定跡の遷移確率も使用する
	use_book_policy = options["Use_Book_Policy"];
    use_book_value = options["Use_Book_Value"];

	// 探索打ち切りを使用する
	use_interruption = options["Use_Interruption"];

	// 評価値の閾値
	book_eval_threshold = options["Book_Eval_Threshold"];

	// 訪問回数の閾値(1000分率)
	book_visit_threshold = options["Book_Visit_Threshold"] / 1000.0;

	book_cutoff = options["Book_Cutoff"] / 1000.0f;

	// 訪問回数に応じてランダムに選択する際の温度パラメータ
	book_reciprocal_temperature = 1000.0 / options["Book_Temperature"];

	// 先手、後手どちらの定跡を作成するか("black":先手、"white":後手、それ以外:両方)
	const Color make_book_color = std::string(options["Make_Book_Color"]) == "black" ? Black : std::string(options["Make_Book_Color"]) == "white" ? White : ColorNum;

	// 定期的にマージする定跡ファイル
	const std::string merge_file = options["Book_Merge_File"];

	SetReuseSubtree(options["ReuseSubtree"]);

	// outFileが存在するときは追加する
	int input_num = 0;
	std::map<Key, std::vector<BookEntry> > outMap;
	{
		std::ifstream ifsOutFile(outFileName.c_str(), std::ios::binary);
		if (ifsOutFile) {
			BookEntry entry;
			while (ifsOutFile.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
				outMap[entry.key].emplace_back(entry);
				input_num++;
			}
			std::cout << "outMap.size: " << outMap.size() << std::endl;
		}
	}

	Position pos(DefaultStartPositionSFEN, thisptr);
	book_starting_pos_key = pos.getKey();

	// 定跡読み込み
	read_book(bookFileName, bookMap);

	// 定跡マージ
	int merged = 0;
	if (merge_file != "") {
		merged += merge_book(outMap, merge_file);
	}

	int black_num = 0;
	int white_num = 0;
	size_t prev_num = outMap.size();
	std::vector<Move> moves;
	for (int trial = 0; trial < limitTrialNum;) {
		// 進捗状況表示
		std::cout << trial << "/" << limitTrialNum << " (" << int((double)trial / limitTrialNum * 100) << "%)" << std::endl;

		// 先手番
		if (make_book_color == Black || make_book_color == ColorNum) {
			int count = 0;
			moves.clear();
			// 探索
			pos.set(DefaultStartPositionSFEN);
			make_book_inner(pos, limits, bookMap, outMap, count, 0, true, moves);
			black_num += count;
			trial++;
		}

		// 後手番
		if (make_book_color == White || make_book_color == ColorNum) {
			int count = 0;
			moves.clear();
			// 探索
			pos.set(DefaultStartPositionSFEN);
			make_book_inner(pos, limits, bookMap, outMap, count, 0, false, moves);
			white_num += count;
			trial++;
		}

		// 完了時およびSave_Book_Intervalごとに途中経過を保存
		if (outMap.size() > prev_num && (trial % save_book_interval == 0 || trial >= limitTrialNum))
		{
			// 定跡マージ
			if (merge_file != "") {
				merged += merge_book(outMap, merge_file);
			}

			prev_num = outMap.size();
			std::ofstream ofs(outFileName.c_str(), std::ios::binary);
			for (auto& elem : outMap) {
				for (auto& elel : elem.second)
					ofs.write(reinterpret_cast<char*>(&(elel)), sizeof(BookEntry));
			}
		}
	}

	// 結果表示
	std::cout << "input\t" << input_num << std::endl;
	std::cout << "black\t" << black_num << std::endl;
	std::cout << "white\t" << white_num << std::endl;
	std::cout << "sum\t" << black_num + white_num << std::endl;
	std::cout << "entries\t" << outMap.size() << std::endl;
	std::cout << "merged entries\t" << merged << std::endl;
}
#endif
