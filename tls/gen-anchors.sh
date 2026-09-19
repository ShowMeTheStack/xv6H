#!/bin/bash
# regenerate tls/trust_anchors.c from tls/roots.pem with BearSSL's brssl tool
# (build it on the laptop: cd BearSSL && make build/brssl).
set -e
cd "$(dirname "$0")"
BRSSL=${BRSSL:-../../BearSSL/build/brssl}
{ echo "// trust anchors generated from tls/roots.pem by 'brssl ta' (see tls/gen-anchors.sh)."
  echo '#include "bearssl.h"'
  $BRSSL ta roots.pem | sed -e 's/^static const br_x509_trust_anchor TAs\[/const br_x509_trust_anchor TAs[/' \
                            -e 's/^#define TAs_NUM *\([0-9]*\)/const size_t TAs_NUM = \1;/'; } > trust_anchors.c
grep -c "^static const unsigned char TA.*_DN" trust_anchors.c
