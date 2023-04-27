/// Manage C-cached subscript arrays for speed.
// Copyright 2022-2023 Berwyn Hoyt. See LICENSE.
// @module yottadb.c

/// Cachearray functions
// @section

#include <stdio.h>
#include <stdbool.h>

#include <libyottadb.h>
#include <lua.h>
#include <lauxlib.h>

#include "yottadb.h"
#include "cachearray.h"

// Number of extra items to allocate in cachearray.
// This prevents having to create a new userdata for every single subnode
// Assumes most uses will only dive into subscripts this much deeper than their starting point node
// If users do go deeper, it all still works; it just has to create a new cachearray
#define ARRAY_OVERALLOC 5

// Raw version of lua_getfield() -- almost negligibly slower than lua_getfield(), but much faster if field not found
#define lua_rawgetfield(L, index, fieldstr) \
  ( lua_pushstring(L, fieldstr), lua_rawget(L, ((index)<0)? ((index)-1): (index)) )
#define lua_rawgetfieldbyindex(L, index, fieldindex) \
  ( lua_pushvalue(L, (fieldindex)), lua_rawget(L, ((index)<0)? ((index)-1): (index)) )

// Update cachearray subscript pointers to match subsdata_string at addr
static void _cachearray_updateaddr(cachearray_t *array, char *subsdata) {
  ydb_buffer_t *element = &array->varname;  // assumes array->subs[] come straight after array->varname
  // unsigned comparison is important below to handle when we get junk data in depth
  int depth = (unsigned)array->depth > (unsigned)array->depth_alloc? array->depth_alloc: array->depth;
  do {
    element->buf_addr = subsdata;
    subsdata += element->len_used;
    element++;
  } while (depth--);
}

// Reallocate (expand) cachearray at stack location `index` and replace it with a new one in the same stack index
// @param index of Lua stack where cachearray resides
// @param new_depth is the required depth in the new cachearray (ARRAY_OVERALLOC will be added to this)
// @param new_subslen is the required subsdata_string length (ARRAY_OVERALLOC*YDB_TYPICAL_SUBLEN will be added)
// @param **subsdata points to where to copy subsdata_string from and where to store the address of the new subsdata_string
// @return cachearray
static cachearray_t *_cachearray_realloc(lua_State *L, int index, int new_depth, int new_subslen, char **subsdata) {
  cachearray_t *array = lua_touserdata(L, index);
  int depth_alloc2 = new_depth + ARRAY_OVERALLOC;
  int arraysize2 = depth_alloc2 * sizeof(ydb_buffer_t);
  int subsdata_alloc2 = new_subslen + (ARRAY_OVERALLOC*YDB_TYPICAL_SUBLEN);
  int allocsize = sizeof(cachearray_t) + arraysize2 + subsdata_alloc2;

  cachearray_t *newarray = lua_newuserdata(L, allocsize);
  newarray->subsdata_alloc = subsdata_alloc2;
  newarray->depth_alloc = depth_alloc2;
  char *new_subsdata = get_subsdata(newarray);  // must set ->subsdata_alloc first
  
  int copydepth = 1 + array->depth_alloc<new_depth? array->depth_alloc: new_depth; // 1+ for varname
  memcpy(&newarray->depth, &array->depth, sizeof(cachearray_t) - offsetof(cachearray_t,depth) + copydepth*sizeof(ydb_buffer_t));

  int copysize = array->subsdata_alloc < new_subslen? array->subsdata_alloc: new_subslen;
  memcpy(new_subsdata, *subsdata, copysize);

  // Set up array sizes before update
  newarray->subsdata_alloc = subsdata_alloc2;
  newarray->depth_alloc = depth_alloc2;
  _cachearray_updateaddr(newarray, new_subsdata);
  *subsdata = new_subsdata;
  lua_replace(L, index-(index<0)); // sub this cachearray onto the stack instead of the original one.
  return newarray;
}

/// Underlying core of `cachearray_create()`, designed to call from C.
// This function may be called by C -- the stack is correct at the end (doesn't depend on Lua's stack fixups).
// For the sake of speed, the caller can pre-allocate `cachearray_t_maxsize` on its stack
// (in which case the returned cachearray will only be valid while inside that C function).
// For example, pre-allocating it on the C stack is 20% (400ns) faster than `lua_newuserdata()`
// @param array_prealloc is a pointer to allocated memory that fits a maximum sized cachearray.
// If it is NULL, the function will allocate cachearray itself, using `lua_newuserdata()`.
// @return cachearray -- lightuserdata if array is supplied in C parameter; otherwise full userdata
int _cachearray_create(lua_State *L, cachearray_t_maxsize *array_prealloc) {
  int args = lua_gettop(L);
  if (lua_type(L, 1) != LUA_TSTRING)
    luaL_error(L, "Cannot generate cachearray: string expected at parameter #1 (varname) (got %s)", lua_typename(L, lua_type(L, 1)));

  // Determine depth of cachearray to malloc
  int type_t1 = lua_type(L, 2);
  int depth2 = args - 1;
  int tlen = 0;
  if (type_t1 == LUA_TTABLE) {
    tlen = luaL_len(L, 2);
    depth2 = tlen + args - 2;
  }
  if (depth2 > YDB_MAX_SUBS)
    luaL_error(L, "Cannot generate cachearray: maximum %d number of subscripts exceeded (got %d)", YDB_MAX_SUBS, depth2);

  // Init array -- leave some room to grow without realloc in case user creates subnodes
  cachearray_t *array;
  // Allocate temporary array on stack for speed
  char _tmp_array[sizeof(cachearray_t) + (depth2+ARRAY_OVERALLOC)*sizeof(ydb_buffer_t) + YDB_LARGE_SUBSLEN];
  if (array_prealloc) {
    array = (cachearray_t *)array_prealloc;
    array->subsdata_alloc = member_size(cachearray_t_maxsize, _subsdata);
    array->depth_alloc = member_len(cachearray_t_maxsize, subs);
  } else {
    array = (cachearray_t *)_tmp_array;
    array->subsdata_alloc = YDB_LARGE_SUBSLEN;
    array->depth_alloc = depth2+ARRAY_OVERALLOC;
  }
  // Push cachearray on the Lua stack -- we will realloc it later if necessary
  lua_pushlightuserdata(L, array);

  char *subsdata = get_subsdata(array);
  int subslen = 0;
  int sub = 0;  // assumes array->subs[] come straight after array->varname

  // Append t1 if it is a table
  char *string;
  size_t len;
  int i = 1;
  for (int arg=1; arg <= args; arg++) {
    if (type_t1==LUA_TTABLE && arg==2) {
      if (i > tlen) continue;
      lua_geti(L, 2, i++);
      arg--;
    } else
      lua_pushvalue(L, arg);
    // Fetch string from top of stack
    string = lua_tolstring(L, -1, &len);
    if (luai_unlikely(!string))
      luaL_error(L, "Cannot generate cachearray: string/number expected in parameter #%d (got %s)", arg, lua_typename(L, lua_type(L, -1)));
    // Reallocate array if necessary
    if (subslen+(int)len > array->subsdata_alloc)
      array = _cachearray_realloc(L, -2, depth2, subslen+len, &subsdata);
    memcpy(subsdata+subslen, string, len);
    lua_pop(L, 1);  // pop appended string

    ydb_buffer_t *element = (ydb_buffer_t*)&array->varname+sub;
    element->buf_addr = subsdata+subslen;
    element->len_used = element->len_alloc = len;
    subslen += len;
    sub++;
  }
  array->depth = depth2;

  if (array == (cachearray_t *)_tmp_array)
    array = _cachearray_realloc(L, -1, depth2, subslen, &subsdata);

  // STACK: varname[, t1], ...], cachearray
  lua_rotate(L, 1, 1);
  lua_pop(L, args);
  return 1;
}

/// Generate and return a C-style array of subscripts as a userdata.
// The resulting full userdata contains a cached C array of varname and subscripts which may be passed
// to raw `_yottadb()` functions as a speedy subsarray.
// @function cachearray
// @usage _yottadb.cachearray(varname[, t1][, ...])
// @param varname is a string: the M glvn. (Note: If t1 is a cachearray, varname is ignored in favour of the one contained in t1)
// @param[opt] t1 is a subsarray table or cachearray to be copied
// @param[opt] ... a list of strings to be appended after t2.
// @return cachearray
int cachearray_create(lua_State *L) {
  return _cachearray_create(L, NULL);
}

/// Append subscripts to an existing cachearray, creating a copy if it is full at this depth.
// Function may be called by C as it tidies up its Lua stack itself
// @function cachearray_append
// @usage _yottadb.cachearray_append(cachearray[, ...])
// @param cachearray is an existing cachearray created by cachearray_create()
// @param[opt] ... a list of strings to be appended
// @return cachearray (the same or new)
int cachearray_append(lua_State *L) {
  int args = lua_gettop(L);
  cachearray_t *array = lua_touserdata(L, 1);
  if (!array)
    luaL_error(L, "Cannot append to cachearray: cachearray expected at parameter #1 (got %s)", lua_typename(L, lua_type(L, 1)));
  // Determine depth of cachearray required (depth2)
  int depth = array->depth;
  int additions = args - 1;
  int depth2 = depth + additions;  // newdepth
  if (depth2 > YDB_MAX_SUBS)
    luaL_error(L, "Cannot append to cachearray: maximum %d number of subscripts exceeded (got %d)", YDB_MAX_SUBS, depth2);
  char *subsdata = get_subsdata(array);

  int subslen = array->subs[array->depth-1].buf_addr + array->subs[array->depth-1].len_used - subsdata;
  // If no room, or next slot already used, then copy parent.__cachearray, but with more space
  if (depth2 > array->depth_alloc || array->depth >= depth2)
    array = _cachearray_realloc(L, 1, depth2, subslen + additions*YDB_TYPICAL_SUBLEN, &subsdata);

  // Append subscripts to array and subsdata_string
  for (int arg=2; arg <= args; arg++) {
    size_t len;
    char *string = lua_tolstring(L, arg, &len);
    if (luai_unlikely(!string))
      luaL_error(L, "Cannot append subscript to cachearray: string/number expected at parameter #%d (got %s)", arg, lua_typename(L, lua_type(L, arg)));
    if (subslen+(int)len > array->subsdata_alloc) {
      // re-allocate userdata now that we know more about actual string lengths required
      int args_left = args-arg;
      array = _cachearray_realloc(L, 1, depth2, subslen+len + args_left*YDB_TYPICAL_SUBLEN, &subsdata);
    }
    // Copy string into subsdata. No need to check max len as we allocated maximum possible
    memcpy(subsdata+subslen, string, len);

    ydb_buffer_t *element = &array->subs[depth];
    element->buf_addr = subsdata+subslen;
    element->len_used = element->len_alloc = len;
    subslen += len;
    depth++;
  }
  array->depth = depth;
  // STACK: cachearray[, ...]
  lua_pop(L, additions);
  return 1;
}

// Make a cachearray mutable
// This is done by removing empty depth slots which makes it
// so it cannot be extended without being copied.
void _cachearray_tomutable(cachearray_t *array) {
  int size_removed = (array->depth_alloc - array->depth) * sizeof(ydb_buffer_t);
  // Now move subsdata down to be just above the array slots
  char *subsdata = get_subsdata(array);
  int subslen = array->subs[array->depth-1].buf_addr + array->subs[array->depth-1].len_used - subsdata;
  array->depth_alloc = array->depth;
  memcpy(subsdata-size_removed, subsdata, subslen);
  // We might as well extend subsdata by that amount since it's already allocated anyway
  array->subsdata_alloc += size_removed;
  _cachearray_updateaddr(array, subsdata-size_removed);
}

// Create a mutable cachearray with changable subscripts -- intended for iteration.
// The only difference from a standard subscript is that free subscript space
// (per ARRAY_OVERALLOC) is not allocated in the cachearray.
// This forces child nodes to create new, immutable cachearrays.
// This is intended only for use by `cachearray_subst()` to efficiently iterate subscripts
// without creating a new cachearray for every single iteration.
// The __mutable field should be set to true by the iterator as a warning to the user
// on any node that has a mutable cachearray.
// @usage _yottadb.cachearray_createmutable(...)
// @param ... parameters are passed directly into `cachearray_create()`
// @return cachearray, subsdata_string
// @see cachearray_create
int cachearray_createmutable(lua_State *L) {
  _cachearray_create(L, NULL);
  cachearray_t *array = lua_touserdata(L, -1);
  _cachearray_tomutable(array);
  return 1;
}

// Substitute final subscript of given mutable cachearray with `string`.
// The supplied cachearray must be created with cachearray_createmutable().
// This is used only by node:subscripts() to efficiently iterate subscripts
// without creating a new cachearray for every single iteration.
// The __mutable field is set by the node.subscripts() iterator as a warning to the
// user on any node that has a mutable cachearray.
// @usage _yottadb.cachearray_subst(cachearray, string)
// @param cachearray userdata
// @param string to store in array index
// @return cachearray (which could be a new mutable one if it could not hold the new subscript size)
int cachearray_subst(lua_State *L) {
  cachearray_t *array = lua_touserdata(L, 1);
  if (!array)
    luaL_error(L, "Parameter #1 to cachearray_subst must be a cachearray userdata");
  int depth = array->depth;
  if (!depth)
    luaL_error(L, "Parameter #1 to cachearray_subst must be a cachearray with at least one subscript");
  if (array->depth_alloc > depth)
    luaL_error(L, "Cachearray must be mutable to run cachearray_subst() on it");
  char *subsdata = get_subsdata(array);
  int subslen = array->subs[depth-1].buf_addr - subsdata;
  size_t len;
  char *string = luaL_checklstring(L, 2, &len);
  if (subslen+(int)len > array->subsdata_alloc) {
    array = _cachearray_realloc(L, 1, depth, subslen+len, &subsdata);
    // set depth_alloc low to lack space so it remains a mutable cachearray that
    // cannot grow subscripts (i.e. must be copied to have more subscripts)
    _cachearray_tomutable(array);
  }
  memcpy(array->subs[depth-1].buf_addr, string, len);
  array->subs[depth-1].len_used = array->subs[depth-1].len_alloc = len;
  lua_pop(L, 1);  // pop string
  return 1;
}

/// Return string of cachearray subscripts or empty string if no subscripts.
// Strings are quoted with %q. Numbers are quoted only if they differ from their string representation.
// @function cachearray_tostring
// @usage _yottadb.cachedump(cachearray[, depth])
// @param cachearray userdata created by _yottadb.cachearray()
// @param[opt] depth of subscripts to show (limits cachearray's inherent length)
// @return subscript_list, varname
int cachearray_tostring(lua_State *L) {
  bool isnum;

  cachearray_t *array = lua_touserdata(L, 1);
  if (!array)
    luaL_error(L, "Parameter #1 to cachearray_tostring must be a cachearray userdata (got %s)", lua_typename(L, lua_type(L, 1)));
  int depth = array->depth;
  int args = lua_gettop(L);
  if (args > 1) depth = luaL_checkinteger(L, 2);
  if (depth < 0 || depth > array->depth)
    luaL_error(L, "Parameter #2 to cachearray_tostring is not a valid node depth in the range 0-%d (got %d)", array->depth, depth);
  lua_pop(L, args-1); // pop [depth, ...]
  // STACK: cachearray
  if (depth == 0) {
    lua_pushstring(L, "");
    goto push_varname;
  }
  ydb_buffer_t *element = &array->subs[0];

  // push string.format
  lua_getglobal(L, "string");
  lua_getfield(L, -1, "format");
  lua_remove(L, -2);  // remove string; leaving string.format()
  // STACK: cachearray, string.format()
  luaL_checkstack(L, depth*2+4, "Lua stack can't grow to fit all cachearray subscript (and separator) strings in cachearray_tostring()");
  for (int len=depth; len--;) {
    lua_pushlstring(L, element->buf_addr, element->len_used);
    lua_pushinteger(L, lua_tointeger(L, -1));
    lua_tostring(L, -1);
    isnum = lua_compare(L, -1, -2, LUA_OPEQ);
    lua_pop(L, 2);  // pop string1, string2
    // invoke: string.format(isnum and "%s" or "%q", <subscript>)
    lua_pushvalue(L, 2);  // push string.format()
    lua_pushstring(L, isnum? "%s": "%q");
    lua_pushlstring(L, element->buf_addr, element->len_used);
    lua_call(L, 2, 1);
    lua_pushstring(L, ",");
    element++;
  }
  lua_pop(L, 1); // remove final comma
  lua_concat(L, depth*2-1);
  // STACK: cachearray, string.format(), retval
  lua_remove(L, -2);
push_varname:
  // STACK: cachearray, retval
  if (array->varname.buf_addr)
    lua_pushlstring(L, array->varname.buf_addr, array->varname.len_used);
  else
    lua_pushnil(L);
  // STACK: cachearray, retval, varname
  lua_remove(L, -3);
  return 2;
}

/// @section end
