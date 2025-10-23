# Create macros for using the lemon parser generator.

# If we're cross-compiling and /usr/share/lemon/lempar.c exists, try to
# find the system lemon and use it. We need to build our own lemon
# otherwise.
if (CMAKE_CROSSCOMPILING AND EXISTS /usr/share/lemon/lempar.c)
	find_program(LEMON_EXECUTABLE lemon)
endif()

if(LEMON_EXECUTABLE)
	# Use system lemon
	macro(generate_lemon_file _out _in)
		add_custom_command(
			OUTPUT
				${_out}.c
				# These files are generated as side-effect
				${_out}.h
				${_out}.out
			COMMAND ${LEMON_EXECUTABLE}
				-T/usr/share/lemon/lempar.c
				-d.
				${_in}
			DEPENDS
				${_in}
		)
	endmacro()
	add_custom_target(lemon)
else()
	# Compile bundled lemon with support for -- to end options
	macro(generate_lemon_file _out _in)
		add_custom_command(
			OUTPUT
				${_out}.c
				# These files are generated as side-effect
				${_out}.h
				${_out}.out
			COMMAND $<TARGET_FILE:lemon>
				-T${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c
				-d.
				--
				${_in}
			DEPENDS
				${_in}
				lemon
				${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c
		)
	endmacro()
endif()

macro(ADD_LEMON_FILES _source _generated)

	foreach (_current_FILE ${ARGN})
		get_filename_component(_in ${_current_FILE} ABSOLUTE)
		get_filename_component(_basename ${_current_FILE} NAME_WE)

		set(_out ${CMAKE_CURRENT_BINARY_DIR}/${_basename})

		generate_lemon_file(${_out} ${_in})

		list(APPEND ${_source} ${_in})
		list(APPEND ${_generated} ${_out}.c)
	endforeach(_current_FILE)
endmacro(ADD_LEMON_FILES)
