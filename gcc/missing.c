
#include <stdlib.h>

// symbols defined by cc1plus but not cc1. we need these stubs so that
// we can use the plugin when compiling both C and CXX files. fortunately
// when this plugin is loaded by cc1plus these will be ignored and the real
// symbols called instead.

void lang_check_failed() { abort(); }
void type_as_string() { abort(); }
void decl_as_string() { abort(); }
void cp_type_quals() { abort(); }
void look_for_overrides_here() { abort(); }
