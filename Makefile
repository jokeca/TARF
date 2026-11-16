.PHONY: all test fit clean
PYTHON ?= python3
JOBS ?= 4
all:
	$(MAKE) -C codes -j$(JOBS)
test: all
	cd codes && bash tests/run_reliability_test.sh
	cd codes && bash tests/run_reliability_profile_test.sh
	cd codes && bash tests/run_reliability_numeric_test.sh
	cd codes && bash tests/run_read_queue_index_test.sh
	$(PYTHON) codes/tests/test_refresh_executor.py
	$(PYTHON) codes/tests/test_partial_refresh.py
	$(PYTHON) codes/tests/test_refresh_reserve.py
	$(PYTHON) codes/tests/test_measurement_phase.py
fit:
	$(PYTHON) experiment_scripts/fit_from_csv.py
clean:
	$(MAKE) -C codes clean
