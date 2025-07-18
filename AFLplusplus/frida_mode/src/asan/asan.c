#include "frida-gumjs.h"

#include "asan.h"
#include "ranges.h"
#include "util.h"

static gboolean asan_enabled = FALSE;
gboolean        asan_initialized = FALSE;

void asan_config(void) {

  if (getenv("AFL_USE_FASAN") != NULL) { asan_enabled = TRUE; }

}

void asan_init(void) {

  FOKF(cBLU "Instrumentation" cRST " - " cGRN "asan:" cYEL " [%c]",
       asan_enabled ? 'X' : ' ');

  if (asan_enabled) {

    asan_arch_init();
    asan_initialized = TRUE;

  }

}

static gboolean asan_exclude_module(GumModule *module, gpointer user_data) {

  gchar                *symbol_name = (gchar *)user_data;
  GumAddress            address;
  const GumMemoryRange *range = gum_module_get_range(module);

  address = gum_module_find_export_by_name(module, symbol_name);
  if (address == 0) { return TRUE; }

  /* If the reported address of the symbol is outside of the range of the module
   * then ignore it */
  if (address < range->base_address) { return TRUE; }
  if (address > (range->base_address + range->size)) { return TRUE; }

  ranges_add_exclude((GumMemoryRange *)range);
  return FALSE;

}

void asan_exclude_module_by_symbol(gchar *symbol_name) {

  gum_process_enumerate_modules(asan_exclude_module, symbol_name);

}

