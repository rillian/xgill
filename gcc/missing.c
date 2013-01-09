
#include <stdlib.h>
#include "xgill.h"

// symbols defined by cc1plus but not cc1. we need these stubs so that
// we can use the plugin when compiling both C and CXX files. fortunately
// when this plugin is loaded by cc1plus these will be ignored and the real
// symbols called instead.

void lang_check_failed() { abort(); }
const char * type_as_string(tree, int) { abort(); return NULL; }
const char * decl_as_string(tree, int) { abort(); return NULL; }
int cp_type_quals(const_tree) { abort(); return 0; }
tree look_for_overrides_here(tree, tree) { abort(); return NULL; }
tree get_template_info(const_tree) { abort(); return NULL; }
