find_package (Threads REQUIRED)

add_library (automation STATIC
			 "automation.cpp"
			 "automation.h"
			 "debugtools.cpp"
			 "debugtools.h")

target_link_libraries (automation PRIVATE core input video log Threads::Threads)
