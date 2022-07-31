# dxruby.so has DXRuby::VERSION, so this file used for pre-build environment (gem build)

module DXRuby
  VERSION = open("#{File.dirname(__FILE__)}/../../ext/dxruby/version.h") { |f|
    f.readlines.find { |l|
      l.match?(/^#define[ \t]+DXRUBY_VERSION[ \t]+/)
    }
  }.slice(/\"([\d\.]+)\"/, 1)
end

