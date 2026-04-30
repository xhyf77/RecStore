import os
import sys

import torch as th

PYTHON_CLIENT_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../../../ps/python_client")
)
if PYTHON_CLIENT_DIR not in sys.path:
    sys.path.insert(0, PYTHON_CLIENT_DIR)

from Adagrad import SparseAdagrad
from DistEmb import DistEmbedding
from DistTensor import DistTensor
from EmbBag import myEmbeddingBag
from PsKvstore import kvinit
from utils import init_prefix_embdim

init_prefix_embdim(1, 32)
kvinit()

init = lambda shape, dtype: th.ones(shape, dtype=dtype)
arr = DistTensor((5, 32), th.float32, init_func=init)
print(arr[0:3])
arr[0:3] = th.ones((3, 32), dtype=th.float32) * 2
print(arr[0:3])
arr2 = DistTensor((5, 32), th.float32, init_func=init)
print(arr2[0:3])

embedding_sum = myEmbeddingBag(6, 32, mode="sum")
data = th.Tensor([[1] * 32, [2] * 32, [3] * 32, [4] * 32, [5] * 32, [6] * 32])
embedding_sum.weight.set_data(data)
input = th.tensor(
    [0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5, 0, 2, 3, 5],
    dtype=th.long,
)
off = th.tensor([0, 2], dtype=th.long)
wei = th.Tensor([0.1] * 32)
print(embedding_sum(input=input, offsets=off, per_sample_weights=wei))
