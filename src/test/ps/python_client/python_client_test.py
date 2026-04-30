import os
import sys

import numpy as np
import torch

PYTHON_CLIENT_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../../../ps/python_client")
)
if PYTHON_CLIENT_DIR not in sys.path:
    sys.path.insert(0, PYTHON_CLIENT_DIR)

from client import GRPCParameterClient

TEST_KEY_SIZE = 12000
TEST_ROUND = 10000
EMB_DIM = 32
SINGLE_TEST_LEN = 1000


if __name__ == "__main__":
    client = GRPCParameterClient("127.0.0.1", 15000, 0, 32)
    keys = torch.arange(0, TEST_KEY_SIZE, 1).to(torch.int64)
    values = torch.rand((TEST_KEY_SIZE, EMB_DIM))
    seed = 42
    torch.manual_seed(seed)
    client.PutParameter(keys, values)

    for i in range(TEST_ROUND):
        if i % 100 == 0:
            put_keys = torch.randint(
                TEST_KEY_SIZE, (SINGLE_TEST_LEN,)
            ).to(torch.int64)
            put_values = torch.rand((SINGLE_TEST_LEN, EMB_DIM))
            client.PutParameter(put_keys, put_values)
            for i in range(SINGLE_TEST_LEN):
                values[put_keys[i]] = put_values[i]
        else:
            get_keys = torch.randint(
                TEST_KEY_SIZE, (SINGLE_TEST_LEN,)
            ).to(torch.int64)
            get_values = client.GetParameter(get_keys)

            if not torch.equal(get_values, values[get_keys]):
                print(get_values)
                print(values[get_keys])
                for i in range(SINGLE_TEST_LEN):
                    if not torch.equal(get_values[i], values[get_keys[i]]):
                        print("Error at index %d" % i)
                        print("key %d" % get_keys[i])
                        print(get_values[i])
                        print(values[get_keys[i]])
                        break
                assert 0

    print("Test passed!")
