CFLAGS+=-std=c++14 
LDFLAGS+=
SOURCES=server-bench.cpp
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=server-bench

.PHONY: all clean

all: $(SOURCES) $(EXECUTABLE)

clean:
	rm -f $(OBJECTS)
	rm -f $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS) 
	$(CXX) $(LDFLAGS) $(OBJECTS) -ltbb -pthread -o $@

.cpp.o:
	$(CXX) $(CFLAGS) -c $< -o $@