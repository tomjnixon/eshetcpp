include(${PROJECT_SOURCE_DIR}/submodules/catch2.cmake)

function(add_eshetcpp_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name}
    PRIVATE
    eshetcpp
    catch2
  )
  add_test(
    NAME ${name}
    COMMAND $<TARGET_FILE:${name}>
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/test"
  )
endfunction()

add_eshetcpp_test(test_action)
add_eshetcpp_test(test_state)
add_eshetcpp_test(test_event)
add_eshetcpp_test(test_msgpack)
