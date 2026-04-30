import json
import multiprocessing
import os
import sys

SCRIPT_DIR = os.path.dirname(__file__)
PYTHON_CLIENT_DIR = os.path.abspath(
    os.path.join(SCRIPT_DIR, "../../../ps/python_client")
)
if PYTHON_CLIENT_DIR not in sys.path:
    sys.path.insert(0, PYTHON_CLIENT_DIR)

from load_client import client


class Args:
    def __init__(self, **kwargs):
        for key, value in kwargs.items():
            setattr(self, key, value)


if __name__ == "__main__":
    num_processes = 8
    processes = []
    args = []
    for i in range(num_processes):
        arg = Args()
        config_path = os.path.join(SCRIPT_DIR, "config", f"config{0}.json")
        with open(config_path) as f:
            config = json.load(f)
        arg.nepochs = config["nepochs"]
        arg.avg_arrival_rate = config["avg_arrival_rate"]
        arg.batch_size = config["batch_size"]
        arg.sub_task_batch_size = config["sub_task_batch_size"]
        arg.embedding_size = config["embedding_size"]
        arg.machine = config["machine"]
        arg.port = config["port"]
        arg.dataset = config["dataset"]
        arg.test = config["test"]
        arg.table_size = config["table_size"]
        args.append(arg)

    for i in range(num_processes):
        process = multiprocessing.Process(target=client, args=(args[i],))
        processes.append(process)
        process.start()

    for process in processes:
        process.join()

    print("All processes have finished")
