# dxruby.so has DXRuby::VERSION, so this file used for pre-build environment (gem build)

module DXRuby
  VERSION = `echo DXRUBY_VERSION | gcc -include #{File.dirname(__FILE__)}/../../ext/dxruby/version.h -E -P -`.chomp.tr('"','')
end

