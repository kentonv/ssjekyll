@0xdd631fd400d857bd;

using Package = import "/sandstorm/package.capnp";

const manifest :Package.Manifest = (
  appVersion = 0,
  actions = [(
    input = (none = void),
    title = (defaultText = "New Jekyll Site"),
    command = (executablePath = "/ssjekyll", args = [])
  )],
  continueCommand = (executablePath = "/ssjekyll", args = [])
);

