CC = /home/truong/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-gcc
LDFLAGS = -lrt -lpthread

all: alarm_core sensor_node

alarm_core: alarm_core.c
	$(CC) -o $@ $^ $(LDFLAGS)

sensor_node: sensor_node.c
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f alarm_core sensor_node
	
	
	
