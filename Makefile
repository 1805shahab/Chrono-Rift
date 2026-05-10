CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

TARGETS = arbiters hips asps

all: $(TARGETS)
	@echo Build complete.

arbiters: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hips: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

asps: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f arbiters hips asps arbiter/arbiter hip/hip asp/asp 2>/dev/null || true

.PHONY: all clean
