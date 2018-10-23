#!/bin/bash

for i in `seq 1 1`; do
	rm -rf *.txt
	./tBrok
	python test.py fd_00_0.txt;
	python test.py fd_00_1.txt;
	python test.py fd_00_2.txt;
	python test.py fd_00_3.txt;
	python test.py fd_01_0.txt;
	python test.py fd_01_1.txt;
	python test.py fd_01_2.txt;
	python test.py fd_01_3.txt;
	python test.py fd_02_0.txt;
	python test.py fd_02_1.txt;
	python test.py fd_02_2.txt;
	python test.py fd_02_3.txt;
	python test.py fd_03_0.txt;
	python test.py fd_03_1.txt;
	python test.py fd_03_2.txt;
	python test.py fd_03_3.txt;
done

