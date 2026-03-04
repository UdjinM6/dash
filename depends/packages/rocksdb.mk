package=rocksdb
$(package)_version=10.10.1
$(package)_download_path=https://github.com/facebook/rocksdb/archive/refs/tags/
$(package)_file_name=v$($(package)_version).tar.gz
$(package)_sha256_hash=df2ff348f3fac8578fd4b727eee7267aaf90cd403c99b55e898d1db63fa8cff5
$(package)_build_subdir=build

define $(package)_set_vars
  $(package)_config_opts=-DCMAKE_BUILD_TYPE=None
  $(package)_config_opts+=-DROCKSDB_BUILD_SHARED=OFF -DROCKSDB_BUILD_STATIC=ON
  $(package)_config_opts+=-DWITH_TESTS=OFF -DWITH_TOOLS=OFF -DWITH_BENCHMARK_TOOLS=OFF
  $(package)_config_opts+=-DWITH_GFLAGS=OFF -DWITH_SNAPPY=OFF -DWITH_ZLIB=OFF -DWITH_BZ2=OFF
  $(package)_config_opts+=-DWITH_LZ4=OFF -DWITH_ZSTD=OFF -DWITH_LIBURING=OFF -DWITH_RUNTIME_DEBUG=OFF
  $(package)_config_opts+=-DPORTABLE=1 -DUSE_RTTI=1
  $(package)_config_opts_mingw32=-DROCKSDB_INSTALL_ON_WINDOWS=ON
endef

define $(package)_config_cmds
  $($(package)_cmake) -S .. -B .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf bin && \
  rm -f lib/*.so* lib/*.dylib* lib/*.dll* lib/*.la
endef
