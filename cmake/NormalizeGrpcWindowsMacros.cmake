# Keep generated declarations and definitions consistent when gRPC dependencies
# include Windows headers that alias the protocol's SendMessage method.
foreach(generated_file IN ITEMS "${A2A_GRPC_SOURCE}" "${A2A_GRPC_HEADER}")
  file(READ "${generated_file}" generated_content)
  set(namespace_marker "namespace lf {")
  string(FIND "${generated_content}" "${namespace_marker}" namespace_position)
  if(namespace_position EQUAL -1)
    message(FATAL_ERROR "Cannot locate A2A namespace in ${generated_file}")
  endif()
  set(macro_guard "// Preserve the protocol method name after all dependency includes.
#ifdef SendMessage
#undef SendMessage
#endif
")
  string(FIND "${generated_content}" "${macro_guard}" guard_position)
  if(guard_position EQUAL -1)
    string(REPLACE "${namespace_marker}" "${macro_guard}${namespace_marker}"
      generated_content "${generated_content}")
    file(WRITE "${generated_file}" "${generated_content}")
  endif()
endforeach()
