// UPX-packed releases: restores the per-section page protections the packer flattened, before any
// application code runs.
#pragma once

// Native and independently packed CDP1 builds leave layout.count == 0: CDP1's loader has already
// restored protections before any original TLS callback or app entry. UPX builds restore here.
void RestorePackedImageProtection();
