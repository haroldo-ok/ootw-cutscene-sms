CXX := g++
CXXFLAGS := -O2 -g -w -std=c++11 -DBYPASS_PROTECTION
RAWGL_OBJS := util.o file.o unpack.o resource.o pak.o resource_nth.o resource_win31.o \
              resource_3do.o video.o graphics_soft.o script.o sfxplayer.o staticres.o \
              bitmap.o screenshot.o scaler.o

awcap: awcap.o $(RAWGL_OBJS)
	$(CXX) -o $@ $^ -lz

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o awcap

.PHONY: clean
