IF(WIN32)

	#SET(Boost_DEBUG 1)
	set(Boost_USE_STATIC_LIBS		ON)
	set(Boost_USE_STATIC_RUNTIME	ON)
	set(BOOST_USE_MULTITHREADED		ON)
	SET(NSCP_GLOBAL_DEFINES ${NSCP_GLOBAL_DEFINES} -DBOOST_ALL_NO_LIB)	# THis is used to disable "automatic linking on windows which seems to break since I dont know how to set link dir

	SET(INC_NSCP_LIBRARYDIR C:/source/lib/x64)
	SET(INC_NSCP_INCLUDEDIR C:/source/include)

	SET(INC_BOOST_INCLUDEDIR "${INC_NSCP_INCLUDEDIR}")
	SET(INC_BOOST_LIBRARYDIR "${INC_NSCP_LIBRARYDIR}")

	SET(INC_OPENSSL_INCLUDEDIR "${INC_NSCP_INCLUDEDIR}")

	SET(INC_PROTOBUF_DIR "c:/source/libraries/protobuf-2.3.0")

	SET(INC_CRYPTOPP_DIR "c:/source/libraries/cryptopp-5.6.0")

	SET(INC_LUA_DIR "C:/source/libraries/lua-5.1.4")
	
	SET(INC_GOOGLE_BREAKPAD_DIR "C:/source/google-breakpad/trunk")

ELSE(WIN32)

	SET(INC_OPENSSL_INCLUDEDIR "/usr/include/")

ENDIF(WIN32)

