#ifndef __SETPCI_H__
#define __SETPCI_H__

enum setpci_commands {
	D3_TO_D0,
	D0_TO_D3,
	HOTRESET_ENABLE,
	HOTRESET_DISABLE,
	SETPCI_MAX
};

int setpci(int argc, char **argv);

#endif /* __SETPCI_H__ */