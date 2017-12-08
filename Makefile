

libdevice_select.so: device_select_layer.cpp
	g++ -o libdevice_select.so device_select_layer.cpp -std=c++11 -shared -fPIC
