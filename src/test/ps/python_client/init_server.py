import argparse
import os
import sys

PYTHON_CLIENT_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../../../ps/python_client")
)
if PYTHON_CLIENT_DIR not in sys.path:
    sys.path.insert(0, PYTHON_CLIENT_DIR)

from client import GRPCParameterClient as Client


def parse():
    parser = argparse.ArgumentParser()
    parser.add_argument("--key_size", type=int, required=True)
    parser.add_argument("--host", type=str, default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15000)
    args = parser.parse_args()
    return args


def main():
    args = parse()
    client = Client(args.host, args.port, 0, 0)
    client.LoadFakeData(args.key_size)
    print("init done")


if __name__ == "__main__":
    main()
