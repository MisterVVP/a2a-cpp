#include <google/protobuf/struct.pb.h>
int main() {
  google::protobuf::Value value;
  (*value.mutable_struct_value()->mutable_fields())["probe"].set_bool_value(true);
  return value.struct_value().fields().size() == 1 ? 0 : 1;
}
