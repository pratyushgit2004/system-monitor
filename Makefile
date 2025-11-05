CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall
TARGET = monitor
SRC = main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
