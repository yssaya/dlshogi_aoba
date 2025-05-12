// 2023 Team AobaZero
// This source code is in the public domain.

#include <numeric>
#include <random>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <exception>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#if !defined(_MSC_VER)
#include <sys/time.h>
#include <unistd.h>
#endif



int fPrt = 1;
const int TMP_BUF_LEN = 256;
void PRT(const char *fmt, ...)
{
	if ( fPrt == 0 ) return;
	va_list ap;
	const int PRT_LEN_MAX = 1024;
	static char text[PRT_LEN_MAX];

	va_start(ap, fmt);
	int len = vsprintf( text, fmt, ap );
	va_end(ap);

	if ( len < 0 || len >= PRT_LEN_MAX ) { fprintf(stderr,"buf over\n"); exit(0); }
	fprintf(stderr,"%s",text);
}

static char debug_str[TMP_BUF_LEN];
void debug_set(const char *file, int line)
{
	char str[TMP_BUF_LEN];
	strncpy(str, file, TMP_BUF_LEN-1);
	const char *p = strrchr(str, '\\');
	if ( p == NULL ) p = file;
	else p++;
	sprintf(debug_str,"%s line=%d\n\n",p,line);
}
void debug_print(const char *fmt, ... )
{
	va_list ap;
	static char text[TMP_BUF_LEN];
	va_start(ap, fmt);
#if defined(_MSC_VER)
	_vsnprintf( text, TMP_BUF_LEN-1, fmt, ap );
#else
	 vsnprintf( text, TMP_BUF_LEN-1, fmt, ap );
#endif
	va_end(ap);
	static char text_out[TMP_BUF_LEN*2];
	sprintf(text_out,"%s%s",debug_str,text);
	PRT("%s\n",text_out);
	exit(0);
}
#define DEBUG_PRT (debug_set(__FILE__,__LINE__), debug_print)


/*
dlshogiの
setoption name DebugMessage value true
で探索させた情報を元にfllodgateのAobaZero_kld_avgが3400以上と対戦した棋譜から2回以上出現した
局面を45万局面30bx384で考えさせた11000局面。AobaZeroの手番局面のみ。

△64歩を突かない。△71銀、△72銀、△62銀、の場合。角をお互い手持ち。
△73桂と跳ねない。△71銀、△72銀、△62銀、の場合。角をお互い手持ち。△32金△33歩、の相掛かりはOK
*/

typedef struct MOVE_LIST {
	std::string move;
	double wr;
	double nnrate;
	int count;
} MOVE_LIST;
const int MOVE_LIST_MAX = 593;
MOVE_LIST move_list[MOVE_LIST_MAX];

const int SFEN_BOOK_MAX = 12000;
std::string sfen_book[SFEN_BOOK_MAX];
std::string sfen_book_best[SFEN_BOOK_MAX];
int sfen_book_num = 0;

#if 0
void read_dlshogi_book(tree_t * restrict ptree)
{
//	FILE *fp = fopen("/home/yss/shogi/dlshogi_book/20240417_173824.log","r");
//	FILE *fp = fopen("/home/yss/prg/dlshogi_book/20240417_173824.log","r");	// 1手5000局面で
	FILE *fp = fopen("/home/yss/shogi/dlshogi_book/20240417_232213.log","r");
//	FILE *fp = fopen("/home/yss/prg/dlshogi_book/20240417_232213.log","r");	// 1手45000局面で
	if ( !fp ) DEBUG_PRT("");
	int n = 0;
	std::string max_m;
	int max_c = 0;
	double max_w = 0;
	double max_a = 0;
	int moves = 0;
	int prev_moves = 0;
	int m_num = 0;
	double ave_wr_sum = 0;
	int ave_wr_n = 0;
	std::string sfen;
	int replace_all = 0;
	int replace_ok = 0;
	int count_position=0,count_bestmove=0;
	for (;;) {
		const int T_LEN = 10000;
		static char str[T_LEN];
		if ( fgets( str, T_LEN-1, fp ) == NULL ) break;	// EOF
//		PRT("%s",str);
		if ( str[0] != '<' && str[1] != '>' ) {
			char *p = strstr(str,"position startpos");
			if ( !p ) break;
//			PRT("%s",str);
			max_c = 0;
			max_w = 0;
			max_a = 0;
			max_m.clear();
			char *q = strstr(str,"m=");
			if ( !q ) DEBUG_PRT("");
			prev_moves = moves;
			moves = atoi(q+2);
			if ( prev_moves != moves ) {
//				PRT("ave_wr=%8.5f(%d)\n",ave_wr_sum / (ave_wr_n + (ave_wr_n==0)),ave_wr_n);
				ave_wr_sum = 0;
				ave_wr_n = 0;
			}
			m_num = 0;
			n++;

			strcpy(str_cmdline, p);
			char *lasts;
			strtok_r( str_cmdline, str_delimiters, &lasts );
			usi_posi( ptree, &lasts );
			sfen = get_sfen_string(ptree, root_turn, 1);
			char tmp[256];
			int sp=0;
			strcpy(tmp,sfen.c_str());
			for(int i=0;i<256;i++) {
				if ( tmp[i]==' ' ) sp++;
				if ( sp==5 ) { tmp[i] = 0; break; }	// 手数を削除
			}
			if ( sp!=5 ) DEBUG_PRT("sfen=%s",sfen.c_str());
			sfen = tmp;
//			print_board(ptree);
// 			for (int y=0;y<9;y++) {	for (int x=0;x<9;x++) {	PRT("%3d",BOARD[y*9+x]); } PRT("\n"); }
 			count_position++;
		}
		if ( str[0]=='<' ) {
			char *p = strstr(str,"move_count:");
			char *q = strstr(str,"nnrate:");
			char *r = strstr(str,"win_rate:");
			char *s = strstr(str,":");
			if ( p && q && r && s ) {
				int c = atoi(p+11);
				double a = atof(q+8);
				double w = atof(r+9);
				char *t = strstr(s+1," ");
				if ( t ) {
					*t = 0;
//					PRT("%5s,%5d,%8.4f,wr=%8.4f\n",s+1,c,a,w);
					if ( c > max_c ) {
						max_c = c;
						max_w = w;
						max_a = a;
						max_m = s+1;
					}
					if ( m_num >= MOVE_LIST_MAX ) DEBUG_PRT("");
					MOVE_LIST *pm = &move_list[m_num];
					pm->wr = w;
					pm->nnrate = a;
					pm->count = c;
					pm->move = s+1;

					m_num++;
				}
			}
			char *u = strstr(str,"bestmove");
			if ( u ) {
//				if ( 1 || strstr(u+9,"resign") ) PRT("%-5s,%5d,%8.4f,wr=%8.4f,m_num=%3d,moves=%3d:bestmove=%s",max_m.c_str(),max_c,max_a,max_w,m_num,moves,u+9);
				ave_wr_sum += max_w;
				ave_wr_n++;
				if ( strncmp(max_m.c_str(), u+9, max_m.size()) != 0 && strstr(u+9,"resign")==NULL ) DEBUG_PRT("max_m.size()=%d",max_m.size());
	 			count_bestmove++;

				int hn[2][8];
				int i;
				for (i=0;i<2;i++) {
					unsigned int hand = HAND_B;	// 先手の持駒
					if ( i==1 )  hand = HAND_W;
					hn[i][1] = (int)I2HandPawn(  hand);
					hn[i][2] = (int)I2HandLance( hand);
					hn[i][3] = (int)I2HandKnight(hand);
					hn[i][4] = (int)I2HandSilver(hand);
					hn[i][5] = (int)I2HandGold(  hand);
					hn[i][6] = (int)I2HandBishop(hand);
					hn[i][7] = (int)I2HandRook(  hand);
				}
				std::string next = "";	// この手が着手可能なら指す
				std::string bad = "";
				// △71銀、△72銀、△62銀、の場合。角をお互い手持ち。△32金△33歩、の相掛かりはOK
				int gin62 = 0;
				if ( BOARD[(1-1)*9+9-7]==-4 || BOARD[(2-1)*9+9-7]==-4 || BOARD[(2-1)*9+9-6]==-4 ) gin62 = 1;
				int has_ka_each = hn[0][6] && hn[1][6];
				if ( gin62 && has_ka_each && BOARD[(3-1)*9+9-6]==-1 ) {	// △63歩がいること
					if ( max_m == "6c6d" && BOARD[(3-1)*9+9-7]==-1 ) {
						next = "7c7d";	// △73歩の場合。△64歩は指さない
						bad  = "8a7c";
					}
					if ( max_m == "8a7c" ) {	// △73桂と跳ねない。早繰り銀に。△63歩がいること
						next = "7b7c";
						if ( BOARD[(2-1)*9+9-6]==-4 ) next = "6b7c";
						bad = "6c6d";
					}
				}
				// △73銀とは上がってくれないか。△54歩と無理やり突くのもありかも。
				// 次善手が△64歩、△73桂の場合あり
				// 角換わりは5手目に▲76歩が形か。37手目まで組みあがると難解すぎて手作業で避けるのは無理
				// 藤井聡太 棋王戦第4局 8手目△62銀から△54歩 position startpos moves 2g2f 8c8d 2f2e 8d8e 7g7f 4a3b 8h7g 7a6b 6i7h 5a4a 7i6h 5c5d
				//   Aobaだと+315。無理。△62銀も+319、無理。
				// 右玉は有力(+120)だが誘導が難しい。王座戦第2局
				
				if ( sfen_book_num >= SFEN_BOOK_MAX ) DEBUG_PRT("");
				sfen_book[sfen_book_num] = sfen;
				sfen_book_best[sfen_book_num] =  max_m;	// 基本は最大回数の手
//				PRT("%s,%s\n",sfen.c_str(),max_m.c_str());
				sfen_book_num++;
				if ( next.size()==0 ) continue;

				replace_all++;
				int next_i = -1;
				// 次善手を探す
				int max_g = 0;
				int max_i = -1;
				for (i=0;i<m_num;i++) {
					MOVE_LIST *pm = &move_list[i];
					if ( pm->move == max_m ) continue;
					if ( pm->move == bad   ) continue;
					if ( pm->move == next ) next_i = i;
		 			if ( pm->count <= max_g ) continue;
		 			max_g = pm->count;
		 			max_i = i;
		 		}

				if ( next_i >=0 || max_i >= 0 ) print_board(ptree);

		 		std::string replace = "";
		 		if ( next_i >= 0 ) {
					MOVE_LIST *pm = &move_list[next_i];
					PRT("next=%-5s,c=%5d,wr=%8.4f, diff wr=%8.4f,c=%5d(%5.3f)\n",next.c_str(),pm->count,pm->wr, max_w - pm->wr, max_c - pm->count, (double)pm->count/max_c );
					if ( fabs(max_w - pm->wr) < 0.02 && (double)pm->count/max_c > 0.2 ) replace = next;
				}
		 		if ( max_i >= 0 ) {
					MOVE_LIST *pm = &move_list[max_i];
					if ( fabs(max_w - pm->wr) < 0.02 && (double)pm->count/max_c > 0.2 && replace.size()==0 && pm->move != "6c6d" ) replace = pm->move;
					if ( replace.size() ) {
						PRT("replace=%-5s,second %-5s,%5d,wr=%8.4f, diff wr=%8.4f,c=%5d(%5.3f)\n",replace.c_str(),pm->move.c_str(),pm->count,pm->wr, max_w - pm->wr, max_c - pm->count, (double)pm->count/max_c );
						PRT("%-5s,%5d,%8.4f,wr=%8.4f,m_num=%3d,moves=%3d:bestmove=%s",max_m.c_str(),max_c,max_a,max_w,m_num,moves,u+9);
						sfen_book_best[sfen_book_num-1] = replace;
						PRT("%s,%s\n",sfen.c_str(),replace.c_str());
						replace_ok++;
					}
				}
			}
		}
	}
	PRT("n=%d,sfen_book_num=%d,replace_ok=(%d/%d),count_position=%d,count_bestmove=%d\n",n,sfen_book_num,replace_ok,replace_all,count_position,count_bestmove);
}
#endif

void read_sfen_best() {
#if 1	// do not use AobaZero book.
	return;
#endif
	FILE *fp = fopen("./../aobasfenbest_20250505.txt","r");
	if ( !fp ) DEBUG_PRT("");
	int n = 0;
	for (;;) {
		const int T_LEN = 10000;
		static char str[T_LEN];
		if ( fgets( str, T_LEN-1, fp ) == NULL ) break;	// EOF
//		PRT("%s",str);
		char *p = strchr(str,'\n');
		if ( !p ) DEBUG_PRT("");
		*p = 0;
		if ( (n&1)==0 ) {
			sfen_book[sfen_book_num] = str;
		}
		if ( (n&1)==1 ) {
			if ( n < 20 ) PRT("%5d:%-5s,%s\n",sfen_book_num,str,sfen_book[sfen_book_num].c_str());
			sfen_book_best[sfen_book_num++] = str;
			if ( strlen(str) < 4 || strlen(str) > 5 || strstr(str,"resign") ) DEBUG_PRT("");
		}
		n++;
	}
	if ( (n&1) ) DEBUG_PRT("");
	PRT("sfen_book_num=%d\n",sfen_book_num);
}

#if 0
typedef struct FLOOD_BEST {
	std::string sfen;
	std::string best;
	int win;
	int all;
	int all_sum;
	int moves;
} FLOOD_BEST;
const int FLOOD_BEST_MAX = 30000;
FLOOD_BEST flood_best[FLOOD_BEST_MAX];
int flood_best_num = 0;

void read_flood_book()
{
//	FILE *fp = fopen("/home/yss/shogi/kifuname/20240425_114649.log","r");
	FILE *fp = fopen("/home/yss/prg/kifuname/20240425_114649.log","r");	// Sora_Ginko 抜きの4400以上、1059局
	if ( !fp ) DEBUG_PRT("");
	for (;;) {
		const int T_LEN = 10000;
		static char str[T_LEN];
		if ( fgets( str, T_LEN-1, fp ) == NULL ) break;	// EOF
//		PRT("%s",str);
		char *p = strstr(str,"position sfen");
		if ( !p ) continue;
		if ( p <= str ) DEBUG_PRT("");
		char *q = strstr(str,":");
		if ( !q ) DEBUG_PRT("");
		char *r = strstr(q+1," ");
		if ( !r ) DEBUG_PRT("");
		*r = 0;
		char *m = strstr(str,"m=");
		if ( !m ) DEBUG_PRT("");
		char *c = strstr(str,"c=");
		if ( !c ) DEBUG_PRT("");
		char *c1 = strstr(c+1,",");
		if ( !c1 ) DEBUG_PRT("");
		char *c2 = strstr(c1+1,"/");
		if ( !c2 ) DEBUG_PRT("");

		std::string sfen = p;
		char tmp[256];
		int sp=0;
		strcpy(tmp,sfen.c_str());
		for(int i=0;i<256;i++) {
			if ( tmp[i]==' ' ) sp++;
			if ( sp==5 ) { tmp[i] = 0; break; }	// 手数を削除
		}
		if ( sp!=5 ) DEBUG_PRT("sfen=%s",sfen.c_str());

		FLOOD_BEST *pf = &flood_best[flood_best_num];
		if ( flood_best_num >= FLOOD_BEST_MAX ) DEBUG_PRT("");
		pf->sfen = tmp;
		pf->best = q+1;
		pf->moves = atoi(m+2);
		pf->all_sum = atoi(c+2);
		pf->win     = atoi(c1+1);
		pf->all     = atoi(c2+1);
		if ( pf->all == 0 || pf->all_sum ==0 || pf->best.size()==0 ) DEBUG_PRT("");
		if ( flood_best_num < 100 ) PRT("%5d:c=%4d,%4d/%4d(%5.3f),m=%3d:%-5s %s\n",flood_best_num,pf->all_sum, pf->win,pf->all, (double)pf->win/pf->all, pf->moves,pf->best.c_str(),pf->sfen.c_str());
		flood_best_num++;
	}
	PRT("flood_best_num=%d\n",flood_best_num);
}

std::string get_flood_book(tree_t * restrict ptree)
{
	std::string sfen = get_sfen_string(ptree, root_turn, 1);
	std::string ret = "";
	int r_sum = 0;
	int r = 0;
	int all_max = 0;
	for (int i=0; i<flood_best_num; i++) {
		FLOOD_BEST *pf = &flood_best[i];
		if ( !strstr(sfen.c_str(), pf->sfen.c_str()) ) continue;
		if ( r_sum == 0 ) {
			r = rand_m521() % pf->all_sum;
		}
		r_sum += pf->all;
//		if ( r <= r_sum && ret=="" ) ret = pf->best;
		PRT("%5d:c=%4d,%4d/%4d(%5.3f),m=%3d:%-5s %s\n",i,pf->all_sum, pf->win,pf->all, (double)pf->win/pf->all, pf->moves,pf->best.c_str(),pf->sfen.c_str());
		if ( pf->all > all_max ) {
			if ( (double)pf->win/pf->all < 0.2                     ) continue;
			if ( (double)pf->win/pf->all < 0.5 && (pf->moves&1)==0 ) continue;
			ret = pf->best;
			all_max = pf->all;
		}
	}
	PRT("r=%d,r_sum=%d,ret=%s\n",r,r_sum,ret.c_str());
	return ret;
}
#endif

void make_dl_book()
{
	read_sfen_best();
//	read_dlshogi_book(ptree);
//	read_flood_book();
}

std::string copy_winner_move(std::string cmdPos)
{
	std::string s = "position " + (std::string)cmdPos;

	typedef struct MANE {
		int winner;		// 先手勝ち 0, 後手勝ち 1
		const char *str;
	} MANE;
	MANE mane[100] = {
		// N+Lightweight, N-dlshogi with HEROZ, 先手を真似する
//		{ 0, "position startpos moves 2g2f 8c8d 2f2e 8d8e 6i7h 4a3b 3i3h 9c9d 9g9f 7a7b 3g3f 8e8f 8g8f 8b8f 5i6h 1c1d 2i3g 7c7d 2e2d 2c2d 2h2d P*2c 2d7d 7b7c P*8g 8f8b 7d7e 7c6d 7e2e 3c3d 7g7f 4c4d 4g4f 3a4b 4i4h 4b4c 3h4g 6a5b 6h5h 2a3c 2e2i 8b7b 7h7g 5c5d 5h6h 2b3a 7i7h 7b8b 1g1f 5a4a 6g6f 8a7c 8g8f 8b8d 7g8g P*7e 6f6e 6d6e 4f4e 5d5e 8h5e 8d7d 7f7e 3a7e P*7f 7e5c 7h6g P*8e P*6d 6e5d 7f7e 7d8d 8f8e 8d8e P*8f 8e8d 5e8h 4d4e 4g5f P*5e 5f5e 5d5e 8h5e 5c6d 5e6d 8d6d S*7d P*7b 7d7c 7b7c N*5f 6d8d P*4d 4c5d B*7f S*6e 7f6e 5d6e S*4c S*4b 8i7g 6e5f 4c3b 4a3b 6g5f P*5h S*2a 3b4a 2i2c+ B*5d 5f6e B*5i 6h5h 5i4h+ 5h4h N*5e B*2i S*4g 2i4g 5e4g+ 4h4g B*6i 4g3h 4a5a P*6b G*4g 3h2i 5a6b N*7d 7c7d 6e5d 4g3g 7g6e 6i4g+ 2i1h N*6a B*4a P*5a P*6d N*7a 6d6c+ 5b6c 2c3b 4g3f 1h2i 3f4g 2i1h 3g2h 1h2h P*2g 2h3i 4g5g G*4h 5g7e 3b4b 7e4b 5d6c 7a6c S*7c 6a7c G*5c 4b5c 6e5c+ 6b5c 4d4c+" },


		{ 0, NULL }
	};

	std::string ret = "";
	for (int loop = 0; ;loop++) {
		if ( mane[loop].str == NULL ) break;
		std::string m = mane[loop].str;
		if ( m.find(s) == std::string::npos ) continue;
		size_t n = s.size();
		std::string m0 = m.substr(n).c_str();
		size_t i = m0.find(" ");
		if ( i == 0 ) {
			m0 = m0.substr(1);
			i = m0.find(" ");
		}
		if ( i == std::string::npos ) continue;
		const char *p = m0.substr(0,i).c_str();
//		PRT("'%s'\n",p);

		int count = 0;
		char ch = ' ';
		for (size_t i = 0; (i = s.find(ch, i)) != std::string::npos; i++) {
			count++;
		}
		if ( (count&1) != mane[loop].winner ) continue;	// 空白2文字、なら先手番。3文字なら後手番
		ret = p;
		PRT("copy winner moves=%d,%s,winner=%d\n",count-2, ret.c_str(), mane[loop].winner);
	}
	return ret;
}

std::string get_dlshogi_book(std::string sfen, std::string latest_pos_cmd)
{
	if ( 0 ) {
		std::string ret = copy_winner_move(latest_pos_cmd);
		if ( ret.size() ) return ret;
	}

	std::mt19937 get_mt_rand;
	std::random_device rd;
	get_mt_rand.seed(rd());
//	if ( (get_mt_rand() % 100) < 80 ) return "";	// floodgateでは Random_Ply=16 を指定して定跡はあまり使わない

	// 初手▲76歩 2025年 Seranade決勝用
//	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b -" ) ) return "7g7f";

#if 0	// 2手目△34歩。先手 +233、8万ノード
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/7P1/PPPPPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "3c3d";
#if 0	// 4手目△32金は横歩取りになるか。先手 +198、8万ノード。それこそ定跡勝負だが・・・。
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P4P1/PP1PPPP1P/1B5R1/LNSGKGSNL w -" )   ) return "4a3b";// ▲26歩△34歩▲76歩
#endif
		// 4手目、6手目に角道を止める。先手 +239、8万ノード
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P4P1/PP1PPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "4c4d";	// 4手目。▲26歩△34歩▲76歩
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r7/ppppppbpp/6p2/7P1/2P6/PP1PPPP1P/1B5R1/LNSGKGSNL w -" )   ) return "4c4d"; // 6手目。▲26歩△34歩▲25歩△33角▲76歩
#endif

#if 0	// 藤井聡太 叡王戦第2局 10手目△33角。▲88銀、▲78銀、▲68銀に対して。先手 +207、8万ノード
	if ( strstr(sfen.c_str(),"position sfen lnsgk1snl/1r4gb1/p1pppp1pp/6p2/1p5P1/2P6/PPBPPPP1P/1S5R1/LN1GKGSNL w -") ) return "2b3c";
	if ( strstr(sfen.c_str(),"position sfen lnsgk1snl/1r4gb1/p1pppp1pp/6p2/1p5P1/2P6/PPBPPPP1P/2S4R1/LN1GKGSNL w -") ) return "2b3c";
	if ( strstr(sfen.c_str(),"position sfen lnsgk1snl/1r4gb1/p1pppp1pp/6p2/1p5P1/2P6/PPBPPPP1P/3S3R1/LN1GKGSNL w -") ) return "2b3c";
#endif

#if 0	// 2手目△62銀。先手 +193、ぐらいか。意外とそれほどひどくない
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/7P1/PPPPPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "7a6b";
#endif
#if 0	// 2手目△14歩。先手 +198、ぐらいか。結構ふざけた2手目でもなんとかなるかも
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/7P1/PPPPPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "1c1d";
#endif
#if 0	// 2手目△94歩。先手 +201、ぐらいか。
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/7P1/PPPPPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "9c9d";
#endif
#if 0	// 2手目△32金。先手  +71、ぐらいか。角換わりにはなりにくい？
	if ( strstr(sfen.c_str(),"position sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/7P1/PPPPPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "4a3b";
	if ( strstr(sfen.c_str(),"position sfen lnsgk1snl/1r4gb1/p1ppppppp/1p7/7P1/2P6/PP1PPPP1P/1B5R1/LNSGKGSNL w -" ) ) return "3c3d";	// 6手目△34歩。△85歩を決めない。横歩に誘導
#endif

	// 24手目▲37桂でなく▲76歩に。やねうら王、2次予選対策
//	if ( strstr(sfen.c_str(),"position sfen ln2k1snl/1rs1g1gb1/p1p1pp3/3p2pRp/9/P5P1P/1PPPPP3/1BG1K1S2/LNS2G1NL b P2p" ) ) return "7g7f";

	std::string ret = "";
	for (int i=0; i<sfen_book_num; i++) {
		if ( !strstr(sfen.c_str(), sfen_book[i].c_str()) ) continue;
//		PRT("sfen_book=%s\n,sfen=%s\n",sfen_book[i].c_str(),sfen.c_str());
		ret = sfen_book_best[i];
		PRT("found book move %s\n",ret.c_str());
		break;
	}
	return ret;
}

