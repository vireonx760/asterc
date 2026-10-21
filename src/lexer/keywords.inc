case UINT32_C(0xd79f909d):
if (length == 6u && memcmp(text, "module", 6u) == 0)
  return AST_TOK_KW_MODULE;
break;
case UINT32_C(0x32608848):
if (length == 6u && memcmp(text, "target", 6u) == 0)
  return AST_TOK_KW_TARGET;
break;
case UINT32_C(0xe4ba733d):
if (length == 7u && memcmp(text, "feature", 7u) == 0)
  return AST_TOK_KW_FEATURE;
break;
case UINT32_C(0x112a90d4):
if (length == 6u && memcmp(text, "import", 6u) == 0)
  return AST_TOK_KW_IMPORT;
break;
case UINT32_C(0x5e25208d):
if (length == 2u && memcmp(text, "as", 2u) == 0)
  return AST_TOK_KW_AS;
break;
case UINT32_C(0xfb080cb3):
if (length == 6u && memcmp(text, "export", 6u) == 0)
  return AST_TOK_KW_EXPORT;
break;
case UINT32_C(0x5127f14d):
if (length == 4u && memcmp(text, "type", 4u) == 0)
  return AST_TOK_KW_TYPE;
break;
case UINT32_C(0x92c2be20):
if (length == 6u && memcmp(text, "struct", 6u) == 0)
  return AST_TOK_KW_STRUCT;
break;
case UINT32_C(0x664fd1d4):
if (length == 5u && memcmp(text, "const", 5u) == 0)
  return AST_TOK_KW_CONST;
break;
case UINT32_C(0xfb9673de):
if (length == 13u && memcmp(text, "static_assert", 13u) == 0)
  return AST_TOK_KW_STATIC_ASSERT;
break;
case UINT32_C(0xfcdd0ccc):
if (length == 7u && memcmp(text, "section", 7u) == 0)
  return AST_TOK_KW_SECTION;
break;
case UINT32_C(0x1dff06ae):
if (length == 6u && memcmp(text, "global", 6u) == 0)
  return AST_TOK_KW_GLOBAL;
break;
case UINT32_C(0x9087ddb7):
if (length == 6u && memcmp(text, "extern", 6u) == 0)
  return AST_TOK_KW_EXTERN;
break;
case UINT32_C(0x6322e9d5):
if (length == 2u && memcmp(text, "fn", 2u) == 0)
  return AST_TOK_KW_FN;
break;
case UINT32_C(0xaa552e6b):
if (length == 8u && memcmp(text, "dispatch", 8u) == 0)
  return AST_TOK_KW_DISPATCH;
break;
case UINT32_C(0x7f778519):
if (length == 4u && memcmp(text, "when", 4u) == 0)
  return AST_TOK_KW_WHEN;
break;
case UINT32_C(0xbdbf5bf0):
if (length == 4u && memcmp(text, "else", 4u) == 0)
  return AST_TOK_KW_ELSE;
break;
case UINT32_C(0x506b03fa):
if (length == 3u && memcmp(text, "let", 3u) == 0)
  return AST_TOK_KW_LET;
break;
case UINT32_C(0xc7e7bc2e):
if (length == 5u && memcmp(text, "store", 5u) == 0)
  return AST_TOK_KW_STORE;
break;
case UINT32_C(0x39386e06):
if (length == 2u && memcmp(text, "if", 2u) == 0)
  return AST_TOK_KW_IF;
break;
case UINT32_C(0x0dc628ce):
if (length == 5u && memcmp(text, "while", 5u) == 0)
  return AST_TOK_KW_WHILE;
break;
case UINT32_C(0xddef486b):
if (length == 4u && memcmp(text, "loop", 4u) == 0)
  return AST_TOK_KW_LOOP;
break;
case UINT32_C(0x85ee37bf):
if (length == 6u && memcmp(text, "return", 6u) == 0)
  return AST_TOK_KW_RETURN;
break;
case UINT32_C(0xc9648178):
if (length == 5u && memcmp(text, "break", 5u) == 0)
  return AST_TOK_KW_BREAK;
break;
case UINT32_C(0xb1727e44):
if (length == 8u && memcmp(text, "continue", 8u) == 0)
  return AST_TOK_KW_CONTINUE;
break;
case UINT32_C(0xeb0cbd62):
if (length == 5u && memcmp(text, "block", 5u) == 0)
  return AST_TOK_KW_BLOCK;
break;
case UINT32_C(0x4f2bc4b5):
if (length == 2u && memcmp(text, "br", 2u) == 0)
  return AST_TOK_KW_BR;
break;
case UINT32_C(0xa8118b37):
if (length == 6u && memcmp(text, "unsafe", 6u) == 0)
  return AST_TOK_KW_UNSAFE;
break;
case UINT32_C(0x529ee39e):
if (length == 7u && memcmp(text, "control", 7u) == 0)
  return AST_TOK_KW_CONTROL;
break;
case UINT32_C(0x0ea0014b):
if (length == 8u && memcmp(text, "comptime", 8u) == 0)
  return AST_TOK_KW_COMPTIME;
break;
case UINT32_C(0xacf38390):
if (length == 3u && memcmp(text, "for", 3u) == 0)
  return AST_TOK_KW_FOR;
break;
case UINT32_C(0x41387a9e):
if (length == 2u && memcmp(text, "in", 2u) == 0)
  return AST_TOK_KW_IN;
break;
case UINT32_C(0x9b8caa55):
if (length == 8u && memcmp(text, "requires", 8u) == 0)
  return AST_TOK_KW_REQUIRES;
break;
case UINT32_C(0xc7535f2e):
if (length == 4u && memcmp(text, "bind", 4u) == 0)
  return AST_TOK_KW_BIND;
break;
case UINT32_C(0x42454824):
if (length == 2u && memcmp(text, "to", 2u) == 0)
  return AST_TOK_KW_TO;
break;
case UINT32_C(0x43f48595):
if (length == 3u && memcmp(text, "reg", 3u) == 0)
  return AST_TOK_KW_REG;
break;
case UINT32_C(0x36a3cad3):
if (length == 5u && memcmp(text, "macro", 5u) == 0)
  return AST_TOK_KW_MACRO;
break;
case UINT32_C(0x2446530a):
if (length == 6u && memcmp(text, "native", 6u) == 0)
  return AST_TOK_KW_NATIVE;
break;
case UINT32_C(0x57b6c998):
if (length == 6u && memcmp(text, "inputs", 6u) == 0)
  return AST_TOK_KW_INPUTS;
break;
case UINT32_C(0xfc881855):
if (length == 7u && memcmp(text, "outputs", 7u) == 0)
  return AST_TOK_KW_OUTPUTS;
break;
case UINT32_C(0x946895a4):
if (length == 5u && memcmp(text, "exact", 5u) == 0)
  return AST_TOK_KW_EXACT;
break;
case UINT32_C(0x65b1d004):
if (length == 5u && memcmp(text, "bytes", 5u) == 0)
  return AST_TOK_KW_BYTES;
break;
case UINT32_C(0xe60759e9):
if (length == 4u && memcmp(text, "load", 4u) == 0)
  return AST_TOK_KW_LOAD;
break;
case UINT32_C(0xaa25504e):
if (length == 4u && memcmp(text, "cast", 4u) == 0)
  return AST_TOK_KW_CAST;
break;
case UINT32_C(0xd55e61e5):
if (length == 5u && memcmp(text, "trunc", 5u) == 0)
  return AST_TOK_KW_TRUNC;
break;
case UINT32_C(0x705624fc):
if (length == 4u && memcmp(text, "zext", 4u) == 0)
  return AST_TOK_KW_ZEXT;
break;
case UINT32_C(0x668d4269):
if (length == 4u && memcmp(text, "sext", 4u) == 0)
  return AST_TOK_KW_SEXT;
break;
case UINT32_C(0x34efc59f):
if (length == 7u && memcmp(text, "bitcast", 7u) == 0)
  return AST_TOK_KW_BITCAST;
break;
case UINT32_C(0x486a370a):
if (length == 7u && memcmp(text, "ptrcast", 7u) == 0)
  return AST_TOK_KW_PTRCAST;
break;
case UINT32_C(0xc9b85840):
if (length == 6u && memcmp(text, "atomic", 6u) == 0)
  return AST_TOK_KW_ATOMIC;
break;
case UINT32_C(0x4db211e5):
if (length == 4u && memcmp(text, "true", 4u) == 0)
  return AST_TOK_KW_TRUE;
break;
case UINT32_C(0x0b069958):
if (length == 5u && memcmp(text, "false", 5u) == 0)
  return AST_TOK_KW_FALSE;
break;
case UINT32_C(0x48b5725f):
if (length == 4u && memcmp(text, "void", 4u) == 0)
  return AST_TOK_KW_VOID;
break;
case UINT32_C(0xc894953d):
if (length == 4u && memcmp(text, "bool", 4u) == 0)
  return AST_TOK_KW_BOOL;
break;
case UINT32_C(0x9338fbb4):
if (length == 2u && memcmp(text, "i8", 2u) == 0)
  return AST_TOK_KW_I8;
break;
case UINT32_C(0x0b42b2f8):
if (length == 2u && memcmp(text, "u8", 2u) == 0)
  return AST_TOK_KW_U8;
break;
case UINT32_C(0x56a2c0db):
if (length == 3u && memcmp(text, "i16", 3u) == 0)
  return AST_TOK_KW_I16;
break;
case UINT32_C(0x79ee1b1f):
if (length == 3u && memcmp(text, "u16", 3u) == 0)
  return AST_TOK_KW_U16;
break;
case UINT32_C(0xcaa7f4a5):
if (length == 3u && memcmp(text, "i32", 3u) == 0)
  return AST_TOK_KW_I32;
break;
case UINT32_C(0x05f374b1):
if (length == 3u && memcmp(text, "u32", 3u) == 0)
  return AST_TOK_KW_U32;
break;
case UINT32_C(0xc69b2266):
if (length == 3u && memcmp(text, "i64", 3u) == 0)
  return AST_TOK_KW_I64;
break;
case UINT32_C(0x79fae712):
if (length == 3u && memcmp(text, "u64", 3u) == 0)
  return AST_TOK_KW_U64;
break;
case UINT32_C(0xcd3f75cd):
if (length == 4u && memcmp(text, "i128", 4u) == 0)
  return AST_TOK_KW_I128;
break;
case UINT32_C(0x90caedf1):
if (length == 4u && memcmp(text, "u128", 4u) == 0)
  return AST_TOK_KW_U128;
break;
case UINT32_C(0x298e017d):
if (length == 5u && memcmp(text, "isize", 5u) == 0)
  return AST_TOK_KW_ISIZE;
break;
case UINT32_C(0x6b9c7a61):
if (length == 5u && memcmp(text, "usize", 5u) == 0)
  return AST_TOK_KW_USIZE;
break;
case UINT32_C(0x003211ca):
if (length == 3u && memcmp(text, "f16", 3u) == 0)
  return AST_TOK_KW_F16;
break;
case UINT32_C(0xa19ca2ea):
if (length == 4u && memcmp(text, "bf16", 4u) == 0)
  return AST_TOK_KW_BF16;
break;
case UINT32_C(0x84375ec4):
if (length == 3u && memcmp(text, "f32", 3u) == 0)
  return AST_TOK_KW_F32;
break;
case UINT32_C(0x00435aeb):
if (length == 3u && memcmp(text, "f64", 3u) == 0)
  return AST_TOK_KW_F64;
break;
case UINT32_C(0x55764c09):
if (length == 3u && memcmp(text, "ptr", 3u) == 0)
  return AST_TOK_KW_PTR;
break;
case UINT32_C(0x027455bb):
if (length == 3u && memcmp(text, "cap", 3u) == 0)
  return AST_TOK_KW_CAP;
break;
case UINT32_C(0xa2622f79):
if (length == 4u && memcmp(text, "bits", 4u) == 0)
  return AST_TOK_KW_BITS;
break;
case UINT32_C(0x733050e5):
if (length == 3u && memcmp(text, "vec", 3u) == 0)
  return AST_TOK_KW_VEC;
break;
case UINT32_C(0xe7774569):
if (length == 4u && memcmp(text, "mask", 4u) == 0)
  return AST_TOK_KW_MASK;
break;
case UINT32_C(0x079ff6c8):
if (length == 4u && memcmp(text, "svec", 4u) == 0)
  return AST_TOK_KW_SVEC;
break;
case UINT32_C(0xc12ee1a6):
if (length == 4u && memcmp(text, "pred", 4u) == 0)
  return AST_TOK_KW_PRED;
break;
case UINT32_C(0x94e1036d):
if (length == 8u && memcmp(text, "volatile", 8u) == 0)
  return AST_TOK_KW_VOLATILE;
break;
