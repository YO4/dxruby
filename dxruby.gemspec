# coding: utf-8
lib = File.expand_path('../lib', __FILE__)
require_relative 'lib/dxruby/version.rb'

Gem::Specification.new do |spec|
  spec.name          = "dxruby"
  spec.version       = DXRuby::VERSION
  spec.authors       = ["mirichi"]
  spec.email         = ["sawara01@gmail.com"]
  spec.description   = %q{2D game library for Windows(DirectX9)}
  spec.summary       = %q{:-)}
  spec.homepage      = "http://dxruby.osdn.jp/"
  spec.license       = "zlib/libpng"

  spec.files         = `git ls-files lib`.split($/)
  spec.executables   = spec.files.grep(%r{^bin/}) { |f| File.basename(f) }
  spec.test_files    = spec.files.grep(%r{^(test|spec|features)/})
  spec.require_paths = ["lib"]

  spec.add_development_dependency "bundler", "~> 1.3"
  spec.add_development_dependency "rake"
end
