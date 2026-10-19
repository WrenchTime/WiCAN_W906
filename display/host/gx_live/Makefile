# display/main/gx.c and the core against the sockets of this PC: the task of the second source talks to a
# real MQTT broker, as it will on the board, and the program writes down what the core makes of it
# (gx_live.c says how to read that). It can connect, subscribe and ping and nothing else, for the same
# reason the display cannot: there is no code in it that builds another packet.
#
#   make                                builds build/gx_live; nothing is run - there is no broker in a build
#   build/gx_live 127.0.0.1 60          a minute against display/tools/mock_flow.py on this PC
#   build/gx_live 192.168.88.2 300      five minutes of listening to the broker of the Cerbo
#   make check                          the whole way on this PC: mock_flow.py as the Cerbo for ten seconds,
#                                       gx_live as the display; then eight seconds against a broker that
#                                       resets every connection. Port 1883 of this PC has to be free
#   make clean
#
# Built on demand only: "make" in display/host/platform does not come by here. The CI builds it
# (.github/workflows/display.yml, job host-tests) and does not run it.
#
# Needs what the simulations of the platform need (../platform/Makefile): a C library with strlcpy().

CC      ?= cc
PYTHON  ?= python3
# As the simulations of the platform are built (../platform/Makefile), with the same stand-ins for the
# headers of ESP-IDF - but for <sys/socket.h>, which steps aside for the one of the PC (SIM_PC_SOCKETS)
STD     := -std=gnu17
CFLAGS  ?= $(STD) -Wall -Wextra -Wimplicit-fallthrough -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
IDF_OFF := -Wno-unused-parameter -Wno-sign-compare -Wno-enum-conversion
EXTRA   ?=

MAIN    := ../../main
CORE    := ../../components/core
IDF     := ../platform/idf
BUILD   := build

INCLUDES := -I$(IDF) -I$(MAIN) -I$(CORE)
# What the second source is made of (display/test/Makefile, SOURCES_cerbo): the rest of the core is not linked
SOURCES := $(addprefix $(CORE)/,cerbo.c mqtt_sub.c values.c json.c)
HEADERS := $(wildcard $(IDF)/*.h $(IDF)/*/*.h $(CORE)/*.h $(MAIN)/*.h)

.PHONY: all check clean

all: $(BUILD)/gx_live

# One session, a bundle a second and none refused: what gx_live says at its end. The mock stops by itself.
check: $(BUILD)/gx_live
	@$(PYTHON) ../../tools/mock_flow.py --host 127.0.0.1 --seconds 14 > $(BUILD)/mock_flow.log 2>&1 & \
	sleep 1; $(BUILD)/gx_live 127.0.0.1 10 > $(BUILD)/gx_live.log 2>&1; status=$$?; wait; \
	tail -n 6 $(BUILD)/gx_live.log; test $$status -eq 0
	@grep -q -E "^sessions 1, bundles taken (9|10|11), " $(BUILD)/gx_live.log
	@grep -q "^refused 0: " $(BUILD)/gx_live.log
	@grep -q "^values in a bundle: 43 to 43; " $(BUILD)/gx_live.log
	@echo "OK: one session, a bundle a second, none refused"
	@# A broker that resets the connection behind its CONNACK (--fault reset): the SUBSCRIBE of the display meets a
	@# socket that was reset, which raises SIGPIPE on a PC. gx_live has to go on - attempts that fail after 0, 2
	@# and 7 s, no session - and end by its time with its summary, not by the signal (status 141).
	@$(PYTHON) ../../tools/mock_flow.py --host 127.0.0.1 --fault reset --seconds 12 > $(BUILD)/mock_reset.log 2>&1 & \
	sleep 1; $(BUILD)/gx_live 127.0.0.1 8 > $(BUILD)/gx_reset.log 2>&1; status=$$?; wait; \
	tail -n 4 $(BUILD)/gx_reset.log; echo "status $$status"; test $$status -eq 0
	@test "$$(grep -c "reset by the broker" $(BUILD)/mock_reset.log)" -ge 2
	@test "$$(grep -c -E "I gx: (not sent|lost)" $(BUILD)/gx_reset.log)" -ge 2
	@grep -q -E "^sessions 0, bundles taken 0, " $(BUILD)/gx_reset.log
	@echo "OK: a broker that resets ends attempts, not the program"

$(BUILD)/gx_live: gx_live.c $(MAIN)/gx.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(IDF_OFF) $(EXTRA) -DSIM_PC_SOCKETS -pthread $(INCLUDES) gx_live.c $(MAIN)/gx.c $(SOURCES) -lm -o $@

clean:
	rm -rf $(BUILD)
