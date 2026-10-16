CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread $(shell mysql_config --cflags)
LDLIBS   = $(shell mysql_config --libs)

all: logger sample_proc

logger: logger.cpp proc_sampler.h idle_monitor.h
	$(CXX) $(CXXFLAGS) -o $@ logger.cpp $(LDLIBS)

sample_proc: sample_proc.cpp proc_sampler.h
	$(CXX) $(CXXFLAGS) -o $@ sample_proc.cpp

clean:
	rm -f logger sample_proc
