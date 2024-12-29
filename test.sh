#!/bin/bash

# pimlex test example

# normal datasets (200M)
# search uniform
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=200000000 --query_num=100000000 --total_num_keys=200000000 --search
# search zipf
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=200000000 --query_num=100000000 --total_num_keys=200000000 --search --sample_distribution=zipf
# insert
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=100000000 --query_num=100000000 --total_num_keys=200000000 --insert
# hotspot insert
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=100000000 --query_num=20000000 --total_num_keys=200000000 --insert_skew
# mix (50%search+50%insert)
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=100000000 --query_num=100000000 --total_num_keys=200000000 --mix --sample_distribution=zipf
# scan
./bin/pimlex_host --keys_file=/home/clx/dataset/genome --init_num_keys=200000000 --query_num=10000000 --total_num_keys=200000000 --scan

# large datasets (800M)
# search uniform
./bin/pimlex_host --keys_file=/home/clx/dataset/books_800M_uint64 --init_num_keys=800000000 --query_num=400000000 --total_num_keys=800000000 --search
# search zipf
./bin/pimlex_host --keys_file=/home/clx/dataset/books_800M_uint64 --init_num_keys=800000000 --query_num=400000000 --total_num_keys=800000000 --search --sample_distribution=zipf
# insert
./bin/pimlex_host --keys_file=/home/clx/dataset/books_800M_uint64 --init_num_keys=400000000 --query_num=400000000 --total_num_keys=800000000 --insert
# mix (50%search+50%insert)
./bin/pimlex_host --keys_file=/home/clx/dataset/books_800M_uint64 --init_num_keys=400000000 --query_num=400000000 --total_num_keys=800000000 --mix --sample_distribution=zipf
