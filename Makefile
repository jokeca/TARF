.PHONY: all test clean
PYTHON ?= python3
JOBS ?= 4
all:
	$(MAKE) -C code_tarf -j$(JOBS)
test: all
	cd code_tarf && bash tests/run_reliability_test.sh
	cd code_tarf && bash tests/run_reliability_profile_test.sh
	cd code_tarf && bash tests/run_reliability_numeric_test.sh
	cd code_tarf && bash tests/run_read_queue_index_test.sh
	$(PYTHON) tests/test_refresh_executor.py
	$(PYTHON) tests/test_partial_refresh.py
	$(PYTHON) tests/test_refresh_reserve.py
	$(PYTHON) tests/test_measurement_phase.py
clean:
	$(MAKE) -C code_tarf clean
