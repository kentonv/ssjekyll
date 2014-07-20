@0x98140dc4d0f49d05;

using Spk = import "/sandstorm/package.capnp";

const pkgdef :Spk.PackageDefinition = (
  id = "nqmcqs9spcdpmqyuxemf0tsgwn8awfvswc58wgk375g4u25xv6yh",

  manifest = (
    appVersion = 6,

    actions = [
      ( title = (defaultText = "New Hacker CMS Site"),
        command = (
          argv = ["/bin/ssjekyll", "-i"],
          environ = [
            (key = "PATH", value = "/usr/local/bin:/usr/bin:/bin"),
            (key = "LD_LIBRARY_PATH", value = "/usr/local/lib:/usr/lib:/lib")])
      )
    ],
    continueCommand = (
      argv = ["/bin/ssjekyll"],
      environ = [
        (key = "PATH", value = "/usr/local/bin:/usr/bin:/bin"),
        (key = "LD_LIBRARY_PATH", value = "/usr/local/lib:/usr/lib:/lib")])
  ),

  sourceMap = (
    searchPath = [
      (sourcePath = "."),
      (sourcePath = "/", hidePaths = [ "home", "proc", "sys", "etc" ]),
      (sourcePath = "/etc/python2.7", packagePath = "etc/python2.7"),
      (sourcePath = "/etc/ld.so.cache", packagePath = "etc/ld.so.cache"),
      (sourcePath = "/etc/localtime", packagePath = "etc/localtime"),
      (sourcePath = "/etc/passwd", packagePath = "etc/passwd"),
      (sourcePath = "/etc/nsswitch.conf", packagePath = "etc/nsswitch.conf"),
      
      (sourcePath = "/var/lib/gems", packagePath = "usr/share/rubygems-integration")
      # Rubygems annoyingly places gems under /var by default, which of course
      # gets overridden by the Sandstorm mutable storage. Apparently Rubygems
      # will also look under /usr/share/rubygems-integration (discovered by
      # running `gem env`), so we map the gems there. (I'm not using the
      # GEM_HOME env variable because it needs to include the Ruby version, so
      # will make this definition file less robust.)
    ]
  ),

  fileList = "sandstorm-files.list",

  alwaysInclude = [
    "client",
    "usr/share/rubygems-integration/1.9.1/gems/pygments.rb-0.5.4",
    "usr/share/rubygems-integration/1.9.1/gems/maruku-0.7.0"
  ]
);

