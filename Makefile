CXX = mpicxx
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra

TARGET = mpi_stage
SRC = mpi_stage.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/
