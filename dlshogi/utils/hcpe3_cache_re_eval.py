import torch
import argparse
from tqdm import tqdm
import numpy as np
from dlshogi import serializers
from dlshogi.network.policy_value_network import policy_value_network
from dlshogi.data_loader import Hcpe3DataLoader
from dlshogi.cppshogi import hcpe3_cache_re_eval, hcpe3_create_cache, hcpe3_reserve_train_data

parser = argparse.ArgumentParser()
parser.add_argument('model')
parser.add_argument('cache')
parser.add_argument('out_cache')
parser.add_argument('--alpha_p', type=float, default=0.95)
parser.add_argument('--alpha_v', type=float, default=0.95)
parser.add_argument('--alpha_r', type=float, default=0.95)
parser.add_argument('--dropoff', type=float, default=0.5)
parser.add_argument('--limit_candidates', type=int, default=10)
parser.add_argument('--batch_size', '-b', type=int, default=1024)
parser.add_argument('--network')
parser.add_argument('--gpu', '-g', type=int, default=0)
parser.add_argument('--use_amp', action='store_true')
parser.add_argument('--amp_dtype', type=str, default='float16', choices=['float16', 'bfloat16'])
args = parser.parse_args()

alpha_p = args.alpha_p
alpha_v = args.alpha_v
alpha_r = args.alpha_r
dropoff = args.dropoff
limit_candidates = args.limit_candidates
batch_size = args.batch_size
use_amp = args.use_amp
amp_dtype = torch.bfloat16 if args.amp_dtype == 'bfloat16' else torch.float16

if args.gpu >= 0:
    device = torch.device(f"cuda:{args.gpu}")
else:
    device = torch.device("cpu")

model = policy_value_network(args.network)
model.to(device)
serializers.load_npz(args.model, model)
model.eval()

data_len, actual_len = Hcpe3DataLoader.load_files([], cache=args.cache)
indexes = np.arange(data_len, dtype=np.uint32)
dataloader = Hcpe3DataLoader(indexes, batch_size, device)

hcpe3_reserve_train_data(data_len)

for i in tqdm(range(0, len(indexes), batch_size)):
    chunk = indexes[i:i + batch_size]
    chunk_size = len(chunk)
    if chunk_size < batch_size:
        chunk_tmp = chunk
        chunk = np.zeros(batch_size, dtype=np.uint32)
        chunk[:chunk_size] = chunk_tmp

    x1, x2, t1, t2, value = dataloader.mini_batch(chunk)
    with torch.cuda.amp.autocast(enabled=use_amp, dtype=amp_dtype):
        with torch.no_grad():
            y1, y2 = model(x1, x2)

            y1 = y1.float().cpu().numpy()
            y2 = y2.float().sigmoid().cpu().numpy()
    if chunk_size < batch_size:
        hcpe3_cache_re_eval(chunk_tmp, y1[:chunk_size], y2[:chunk_size], alpha_p, alpha_v, alpha_r, dropoff, limit_candidates)
    else:
        hcpe3_cache_re_eval(chunk, y1, y2, alpha_p, alpha_v, alpha_r, dropoff, limit_candidates)

hcpe3_create_cache(args.out_cache)
