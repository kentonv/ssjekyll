CXX=clang++
CXXFLAGS=-O2 -Wall
CXXFLAGS2=-std=c++1y -Itmp $(CXXFLAGS)
CAPNP_DIR=/usr/local/include

.PHONEY: all install uninstall clean environment

ssjekyll.spk: pkg/sandstorm-manifest pkg/ssjekyll pkg/client pkg/dev pkg/var pkg/tmp secret.key pkg/usr/bin/ruby
	spk pack pkg secret.key ssjekyll.spk

clean:
	@# We intentionally don't remove secret.key.
	rm -rf tmp pkg ssjekyll.spk

secret.key:
	spk keygen secret.key

tmp/genfiles:
	@mkdir -p tmp
	capnp compile --src-prefix=$(CAPNP_DIR) -oc++:tmp $(CAPNP_DIR)/sandstorm/*.capnp
	@touch tmp/genfiles

pkg/sandstorm-manifest: manifest.capnp
	@mkdir -p pkg tmp
	capnp eval -b manifest.capnp manifest > tmp/sandstorm-manifest
	mv tmp/sandstorm-manifest pkg/sandstorm-manifest

pkg/ssjekyll: tmp/genfiles server/ssjekyll.c++
	@mkdir -p pkg
	$(CXX) -static server/ssjekyll.c++ tmp/sandstorm/*.capnp.c++ -o pkg/ssjekyll $(CXXFLAGS2) `pkg-config capnp-rpc --cflags --libs`

pkg/client: client/*
	@mkdir -p pkg
	cp -rL client pkg/client

pkg/tmp:
	mkdir pkg/tmp
pkg/var:
	mkdir pkg/var
pkg/dev:
	mkdir pkg/dev
pkg/usr/bin/ruby: copy-package-files.sh
	./copy-package-files.sh

