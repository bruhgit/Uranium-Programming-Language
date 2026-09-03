namespace Sistem {
    public struct Config {
        public static let version = "1.0"
        public static fn getVersion() {
            return "Version: " + this.version
        }
        public static fn getVersion2() {
            return "Version2: " + Config.version
        }
    }
}
printn(Sistem.Config.getVersion())
printn(Sistem.Config.getVersion2())
