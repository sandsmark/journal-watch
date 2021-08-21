CCFILES=$(wildcard *.cpp)
CXXFLAGS+=-g -fPIC -std=c++2a -Wall -Wextra -pedantic
OBJECTS=$(patsubst %.cpp, %.o, $(CCFILES))
LDFLAGS+=-lsystemd -g

#CXXFLAGS += -fsanitize=undefined -fsanitize=address
#LDFLAGS += -fsanitize=undefined -fsanitize=address

journal-watch: $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) -MMD -MP $(CXXFLAGS) -o $@ -c $<

DEPS=$(OBJECTS:.o=.d)
-include $(DEPS)

clean:
	rm -f journal-watch $(OBJECTS) $(DEPS)
