LIBXML2_VERSION=2.14.5

build/inst/%/lib/pkgconfig/libxml-2.0.pc: build/libxml2-$(LIBXML2_VERSION)/build-%/Makefile
	-cd build/libxml2-$(LIBXML2_VERSION)/build-$* && \
		$(MAKE)
	cd build/libxml2-$(LIBXML2_VERSION)/build-$* && \
		$(MAKE) install

build/libxml2-$(LIBXML2_VERSION)/build-%/Makefile: build/libxml2-$(LIBXML2_VERSION)/configure | build/inst/%/cflags.txt
	mkdir -p build/libxml2-$(LIBXML2_VERSION)/build-$*
	cd build/libxml2-$(LIBXML2_VERSION)/build-$* && \
		emconfigure ../../libxml2-$(LIBXML2_VERSION)/configure \
			--prefix="$(PWD)/build/inst/$*" \
			--disable-shared --enable-static \
			--without-python --without-lzma \
			CFLAGS="-Oz `cat $(PWD)/build/inst/$*/cflags.txt`"

extract: build/libxml2-$(LIBXML2_VERSION)/configure

build/libxml2-$(LIBXML2_VERSION)/configure: build/libxml2-$(LIBXML2_VERSION).tar.xz
	cd build && tar xf libxml2-$(LIBXML2_VERSION).tar.xz
	touch $@

build/libxml2-$(LIBXML2_VERSION).tar.xz:
	mkdir -p build
	curl -L https://download.gnome.org/sources/libxml2/2.14/libxml2-$(LIBXML2_VERSION).tar.xz -o $@

libxml2-release:
	cp build/libxml2-$(LIBXML2_VERSION).tar.xz dist/release/libav.js-$(LIBAVJS_VERSION)/sources/

.PRECIOUS: \
	build/inst/%/lib/pkgconfig/libxml-2.0.pc \
	build/libxml2-$(LIBXML2_VERSION)/build-%/Makefile \
	build/libxml2-$(LIBXML2_VERSION)/configure
