@0x98140dc4d0f49d05;

using Spk = import "/sandstorm/package.capnp";

const pkgdef :Spk.PackageDefinition = (
  id = "nqmcqs9spcdpmqyuxemf0tsgwn8awfvswc58wgk375g4u25xv6yh",

  manifest = (
    appVersion = 2,

    actions = [
      ( title = (defaultText = "New Hacker CMS Site"),
        command = (
          argv = ["/bin/ssjekyll", "-i"],
          environ = [(key = "PATH", value = "/usr/local/bin:/usr/bin:/bin")])
      )
    ],
    continueCommand = (
      argv = ["/bin/ssjekyll"],
      environ = [(key = "PATH", value = "/usr/local/bin:/usr/bin:/bin")])
  ),

  sourceMap = (
    searchPath = [
      (sourcePath = "."),
      (sourcePath = "/", hidePaths = [ "home", "proc", "sys" ]),
      
      # Rubygems annoyingly places gems under /var by default, which of course
      # gets overridden by the Sandstorm mutable storage. Apparently Rubygems
      # will also look under /usr/share/rubygems-integration (discovered by
      # running `gem env`), so we map the gems there. (I'm not using the
      # GEM_HOME env variable because it needs to include the Ruby version, so
      # will make this definition file less robust.)
      (sourcePath = "/var/lib/gems", packagePath = "usr/share/rubygems-integration")
    ]
  ),

  fileList = "sandstorm-files.list"
);

