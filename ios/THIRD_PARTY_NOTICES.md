# iOS third-party notices

The distribution target uses SameBoy under its applicable license and embeds the licenses shipped in
`deps/sameboy`. The control-plane runtime uses QuickJS/txiki.js and rpcpp; retain their upstream notices
when preparing an archive.

Mesen and its GPL-covered code are desktop dependencies and are excluded from the iOS archive. mGB and
all other cartridge ROMs are also excluded from distribution builds. Users must import ROMs they are
licensed to use. A publisher may enable the development-only mGB embedding switch for local testing,
but must not submit that artifact without compatible redistribution permission.
