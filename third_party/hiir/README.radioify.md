# HIIR coefficient designer

Laurent de Soras's half-band polyphase IIR coefficient designer, from the
header-only mirror at <https://github.com/unevens/hiir>, pinned to commit
`4589fedb4d08b899514cb605ccd7418bf262ab18`.

Only `PolyphaseIir2Designer.h`, `def.h`, `fnc.h`, `fnc.hpp`, and `license.txt`
are included, unchanged. They are licensed under WTFPL version 2; each source
retains its author's notice. No runtime download or external binary is used.
Radioify calls the designer during initialization and owns the streaming state
in `audio/audiofilter/math/oversampling.h`.
