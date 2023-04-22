import argparse
from os import path
import sys

from morastrja import count_all


def run():
    from time import time

    parser = argparse.ArgumentParser(
        prog='morastrja',
        description='Count morae in file')
    parser.add_argument(
        'filename', nargs='?', default='',
        help='input file to analyze. If omitted, standard input is used.')
    parser.add_argument(
        '-v', '--validate', action='store_true',
        help='whether to validate the input or not')
    ns = parser.parse_args()

    filename = ns.filename
    if not filename:
        s = input()
        if ns.validate:
            mora_cnt = count_all(s)
        else:
            mora_cnt = count_all(s, ignore=True)
        print(f"$ total mora count: {mora_cnt}")
    else:
        start_time = time()
        with open(filename, 'r', encoding='utf-8') as r:
            s = r.read()
        s = s.replace('\n', '')
        if ns.validate:
            mora_cnt = count_all(s)
        else:
            mora_cnt = count_all(s, ignore=True)
        timing = (time() - start_time) * 1000
        print(f"$ source file: {path.abspath(filename)}\n"
              f"$ processing time: {timing} ms\n"
              f"$ total mora count: {mora_cnt}")

run()
