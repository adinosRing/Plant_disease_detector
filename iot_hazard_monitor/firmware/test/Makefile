CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

SRC := test_hazard_logic.cpp ../hazard_monitor/hazard_logic.cpp

test: test_hazard_logic
	./test_hazard_logic

test_hazard_logic: $(SRC) ../hazard_monitor/hazard_logic.h ../hazard_monitor/mq2_math.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f test_hazard_logic

.PHONY: test clean
