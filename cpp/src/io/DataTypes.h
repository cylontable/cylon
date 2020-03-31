#ifndef TWISTERX_SRC_IO_DATATYPES_H_
#define TWISTERX_SRC_IO_DATATYPES_H_

namespace twisterx {
namespace io {
enum DataTypes {
  BIGINT = 0,
  BIT = 1,
  DATEDAY = 2,
  DATEMILLI = 3,
  DECIMAL = 4,
  DURATION = 5,
  EXTENSIONTYPE = 38,
  FIXED_SIZE_LIST = 6,
  FIXEDSIZEBINARY = 7,
  FLOAT4 = 8,
  FLOAT8 = 9,
  INT = 10,
  INTERVALDAY = 11,
  INTERVALYEAR = 12,
  LIST = 13,
  MAP = 14,
  NULL = 15,
  SMALLINT = 16,
  STRUCT = 17,
  TIMEMICRO = 18,
  TIMEMILLI = 19,
  TIMENANO = 20,
  TIMESEC = 21,
  TIMESTAMPMICRO = 22,
  TIMESTAMPMICROTZ = 23,
  TIMESTAMPMILLI = 24,
  TIMESTAMPMILLITZ = 25,
  TIMESTAMPNANO = 26,
  TIMESTAMPNANOTZ = 27,
  TIMESTAMPSEC = 28,
  TIMESTAMPSECTZ = 29,
  TINYINT = 30,
  UINT1 = 31,
  UINT2 = 32,
  UINT4 = 33,
  UINT8 = 34,
  UNION = 35,
  VARBINARY = 36,
  VARCHAR = 37
};
}
}

#endif //TWISTERX_SRC_IO_DATATYPES_H_
