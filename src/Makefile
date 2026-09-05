CXX = $(HOME)/gcc16/bin/g++
CXXFLAGS = -freflection -std=c++26 -g -O2 -I/usr/include/jsoncpp -I. -Icppgow/include
LDFLAGS = -ljsoncpp -Lcppgow -Wl,-rpath,$(CURDIR)/cppgow -Wl,-rpath,$(HOME)/gcc16/lib64 -lcppgowcxx -lcppgow -lasound -lpthread

OBJS = main.o rest.o audio.o midi.o

mucoto: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

main.o: main.cc rest.hh jsonser.hh audio.hh midi.hh b64.hh
rest.o: rest.cc rest.hh jsonser.hh
audio.o: audio.cc audio.hh
midi.o: midi.cc midi.hh audio.hh

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f mucoto $(OBJS)

.PHONY: clean
