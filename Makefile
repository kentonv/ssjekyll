CXX=clang++
CXXFLAGS=-O2 -Wall
CXXFLAGS2=-std=c++1y -Itmp $(CXXFLAGS)
CAPNP_DIR=/opt/sandstorm/latest/usr/include

.PHONEY: all install uninstall clean environment

ssjekyll.spk: bin/ssjekyll sandstorm-pkgdef.capnp sandstorm-files.list
	spk pack ssjekyll.spk

clean:
	rm -rf tmp bin/ssjekyll ssjekyll.spk

tmp/genfiles:
	@mkdir -p tmp
	capnp compile --src-prefix=$(CAPNP_DIR) -oc++:tmp $(CAPNP_DIR)/sandstorm/*.capnp
	@touch tmp/genfiles

bin/ssjekyll: tmp/genfiles server/ssjekyll.c++
	@mkdir -p bin
	$(CXX) -static server/ssjekyll.c++ tmp/sandstorm/*.capnp.c++ -o bin/ssjekyll $(CXXFLAGS2) `pkg-config capnp-rpc --cflags --libs`

