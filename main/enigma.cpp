#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <libsig_comp.h>
#include <linux/dvb/version.h>

#include <lib/actions/action.h>
#include <lib/driver/rc.h>
#include <lib/base/ioprio.h>
#include <lib/base/e2avahi.h>
#include <lib/base/ebase.h>
#include <lib/base/eenv.h>
#include <lib/base/eerror.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>
#include <lib/gdi/gmaindc.h>
#include <lib/gdi/glcddc.h>
#include <lib/gdi/grc.h>
#include <lib/gdi/epng.h>
#include <lib/gdi/font.h>
#include <lib/gui/ebutton.h>
#include <lib/gui/elabel.h>
#include <lib/gui/elistboxcontent.h>
#include <lib/gui/ewidget.h>
#include <lib/gui/ewidgetdesktop.h>
#include <lib/gui/ewindow.h>
#include <lib/gui/evideo.h>
#include <lib/python/connections.h>
#include <lib/python/python.h>
#include <lib/python/pythonconfig.h>
#include <lib/service/servicepeer.h>

#include "bsod.h"
#include "version_info.h"

#ifdef OBJECT_DEBUG
int object_total_remaining;

void object_dump()
{
	printf("%d items left\n", object_total_remaining);
}
#endif

static eWidgetDesktop *wdsk, *lcddsk;

static int prev_ascii_code;

int getPrevAsciiCode()
{
	int ret = prev_ascii_code;
	prev_ascii_code = 0;
	return ret;
}

void keyEvent(const eRCKey &key)
{
	static eRCKey last(0, 0, 0);
	static int num_repeat;

	ePtr<eActionMap> ptr;
	eActionMap::getInstance(ptr);

	if ((key.code == last.code) && (key.producer == last.producer) && key.flags & eRCKey::flagRepeat)
		num_repeat++;
	else
	{
		num_repeat = 0;
		last = key;
	}

	if (num_repeat == 4)
	{
		ptr->keyPressed(key.producer->getIdentifier(), key.code, eRCKey::flagLong);
		num_repeat++;
	}

	if (key.flags & eRCKey::flagAscii)
	{
		prev_ascii_code = key.code;
		ptr->keyPressed(key.producer->getIdentifier(), 510 /* faked KEY_ASCII */, 0);
	}
	else
		ptr->keyPressed(key.producer->getIdentifier(), key.code, key.flags);
}

/************************************************/
#include <lib/components/scan.h>
#include <lib/dvb/idvb.h>
#include <lib/dvb/dvb.h>
#include <lib/dvb/db.h>
#include <lib/dvb/dvbtime.h>
#include <lib/dvb/epgcache.h>
#include <lib/dvb/epgtransponderdatareader.h>

/* Defined in eerror.cpp */
void setDebugTime(bool enable);

class eMain : public eApplication, public sigc::trackable
{
	eInit init;
	ePythonConfigQuery config;

	ePtr<eDVBDB> m_dvbdb;
	ePtr<eDVBResourceManager> m_mgr;
	ePtr<eDVBLocalTimeHandler> m_locale_time_handler;
	ePtr<eEPGCache> m_epgcache;
	ePtr<eEPGTransponderDataReader> m_epgtransponderdatareader;

public:
	eMain()
	{
		e2avahi_init(this);
		init_servicepeer();
		init.setRunlevel(eAutoInitNumbers::main);
		/* TODO: put into init */
		m_dvbdb = new eDVBDB();
		m_mgr = new eDVBResourceManager();
		m_locale_time_handler = new eDVBLocalTimeHandler();
		m_epgcache = new eEPGCache();
		m_epgtransponderdatareader = new eEPGTransponderDataReader();
		m_mgr->setChannelList(m_dvbdb);
	}

	~eMain()
	{
		m_dvbdb->saveServicelist();
		m_mgr->releaseCachedChannel();
		done_servicepeer();
		e2avahi_close();
	}
};

int exit_code;

void quitMainloop(int exitCode)
{
	FILE *f = fopen("/proc/stb/fp/was_timer_wakeup", "w");
	if (f)
	{
		fprintf(f, "%d", 0);
		fclose(f);
	}
	else
	{
		int fd = open("/dev/dbox/fp0", O_WRONLY);
		if (fd >= 0)
		{
			if (ioctl(fd, 10 /*FP_CLEAR_WAKEUP_TIMER*/) < 0)
				eDebug("[quitMainloop] FP_CLEAR_WAKEUP_TIMER failed: %m");
			close(fd);
		}
		else
			eDebug("[quitMainloop] open /dev/dbox/fp0 for wakeup timer clear failed: %m");
	}
	exit_code = exitCode;
	eApp->quit(0);
}

void pauseInit()
{
	eInit::pauseInit();
}

void resumeInit()
{
	eInit::resumeInit();
}

static void sigterm_handler(int num)
{
	quitMainloop(128 + num);
}

void catchTermSignal()
{
	struct sigaction act = {};

	act.sa_handler = sigterm_handler;
	act.sa_flags = SA_RESTART;

	if (sigemptyset(&act.sa_mask) == -1)
		perror("sigemptyset");
	if (sigaction(SIGTERM, &act, 0) == -1)
		perror("SIGTERM");
}

int main(int argc, char **argv)
{
#ifdef MEMLEAK_CHECK
	atexit(DumpUnfreed);
#endif

#ifdef OBJECT_DEBUG
	atexit(object_dump);
#endif

	// set pythonpath if unset
	setenv("PYTHONPATH", eEnv::resolve("${libdir}/enigma2/python").c_str(), 0);
	printf("PYTHONPATH: %s\n", getenv("PYTHONPATH"));
	printf("DVB_API_VERSION %d DVB_API_VERSION_MINOR %d\n", DVB_API_VERSION, DVB_API_VERSION_MINOR);

	// get enigma2 debug level settings
	debugLvl = getenv("ENIGMA_DEBUG_LVL") ? atoi(getenv("ENIGMA_DEBUG_LVL")) : 4;
	if (debugLvl < 0)
		debugLvl = 0;
	printf("ENIGMA_DEBUG_LVL=%d\n", debugLvl);
	if (getenv("ENIGMA_DEBUG_TIME"))
		setDebugTime(atoi(getenv("ENIGMA_DEBUG_TIME")) != 0);

	ePython python;
	eMain main;

	ePtr<gMainDC> my_dc;
	gMainDC::getInstance(my_dc);

	// int double_buffer = my_dc->haveDoubleBuffering();

	ePtr<gLCDDC> my_lcd_dc;
	gLCDDC::getInstance(my_lcd_dc);

		/* ok, this is currently hardcoded for arabic. */
			/* some characters are wrong in the regular font, force them to use the replacement font */
	for (int i = 0x60c; i <= 0x66d; ++i)
		eTextPara::forceReplacementGlyph(i);
	eTextPara::forceReplacementGlyph(0xfdf2);
	for (int i = 0xfe80; i < 0xff00; ++i)
		eTextPara::forceReplacementGlyph(i);

	eWidgetDesktop dsk(my_dc->size());
	eWidgetDesktop dsk_lcd(my_lcd_dc->size());

	dsk.setStyleID(0);
	dsk_lcd.setStyleID(1);

	/*
	if (double_buffer)
	{
		eDebug("[MAIN] - double buffering found, enable buffered graphics mode.");
		dsk.setCompositionMode(eWidgetDesktop::cmBuffered);
	}
	*/

	wdsk = &dsk;
	lcddsk = &dsk_lcd;

	dsk.setDC(my_dc);
	dsk_lcd.setDC(my_lcd_dc);

	dsk.setBackgroundColor(gRGB(0, 0, 0, 0xFF));

	/* redrawing is done in an idle-timer, so we have to set the context */
	dsk.setRedrawTask(main);
	dsk_lcd.setRedrawTask(main);


	eDebug("[MAIN] Loading spinners...");

	{
		int i;
#define MAX_SPINNER 64
		ePtr<gPixmap> wait[MAX_SPINNER];
		for (i=0; i<MAX_SPINNER; ++i)
		{
			char filename[64] = {};
			std::string rfilename;
			snprintf(filename, sizeof(filename), "${datadir}/enigma2/skin_default/spinner/wait%d.png", i + 1);
			rfilename = eEnv::resolve(filename);

			if (::access(rfilename.c_str(), R_OK) < 0)
				break;

			loadImage(wait[i], rfilename.c_str());
			if (!wait[i])
			{
				eDebug("[MAIN] failed to load %s: %m", rfilename.c_str());
				break;
			}
		}
		eDebug("[MAIN] found %d spinner!", i);
		if (i)
			my_dc->setSpinner(eRect(ePoint(100, 100), wait[0]->size()), wait, i);
		else
			my_dc->setSpinner(eRect(100, 100, 0, 0), wait, 1);
	}

	gRC::getInstance()->setSpinnerDC(my_dc);

	eRCInput::getInstance()->keyEvent.connect(sigc::ptr_fun(&keyEvent));

	printf("[MAIN] executing main\n");

	bsodCatchSignals();
	catchTermSignal();

	setIoPrio(IOPRIO_CLASS_BE, 3);

	/* start at full size */
	eVideoWidget::setFullsize(true);

	python.execFile(eEnv::resolve("${libdir}/enigma2/python/StartEnigma.py").c_str());

	/* restore both decoders to full size */
	eVideoWidget::setFullsize(true);

	if (exit_code == 5) /* python crash */
	{
		eDebug("[MAIN] (exit code 5)");
		bsodFatal(0);
	}

	dsk.paint();
	dsk_lcd.paint();

	{
		gPainter p(my_lcd_dc);
		p.resetClip(eRect(ePoint(0, 0), my_lcd_dc->size()));
		p.clear();
		p.flush();
	}

	return exit_code;
}

eWidgetDesktop *getDesktop(int which)
{
	return which ? lcddsk : wdsk;
}

eApplication *getApplication()
{
	return eApp;
}

void runMainloop()
{
	catchTermSignal();
	eApp->runLoop();
}

const char *getEnigmaVersionString()
{
	return enigma2_version;
}

const char *getBoxType()
{
	return BOXTYPE;
}

#include <malloc.h>

void dump_malloc_stats(void)
{
#ifdef __GLIBC__
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)
	struct mallinfo2 mi = mallinfo2();
#if HAVE_AMLOGIC
	eDebug("MALLOC: %ld total", mi.uordblks);

#else
	eDebug("MALLOC: %zu total", mi.uordblks);
#endif
#else
	struct mallinfo mi = mallinfo();
	eDebug("MALLOC: %d total", mi.uordblks);
#endif
#else
	eDebug("MALLOC: info not exposed");
#endif
}

std::string getActiveAdapter()
{
	std::string ret = "";
	struct ifaddrs *ifaddr, *ifa;
	int status;
	// Get the list of network interfaces
	status = getifaddrs(&ifaddr);
	if (status != 0)
	{
		eDebug("[Enigma] getActiveAdapter: Failed to get network interfaces.");
		return "";
	}
	// Iterate through the network interfaces
	for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == nullptr)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK) // ignore loopback
			continue;
		// Check if the interface is active and has an IP address
		if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_addr->sa_family == AF_INET ||
										  ifa->ifa_addr->sa_family == AF_INET6))
		{

			if (strstr(ifa->ifa_name, "eth") || strstr(ifa->ifa_name, "wlan"))
			{
				eDebug("[Enigma] getActiveAdapter: Active network interface: %s.", ifa->ifa_name);
				ret = ifa->ifa_name;
				break;
			}
		}
	}
	freeifaddrs(ifaddr);
	return ret;
}

int checkLinkStatus()
{
	std::string interface = getActiveAdapter();
	if (interface.empty())
	{
		eDebug("[Enigma] checkLinkStatus: No valid active network adapter.");
		return 4;
	}

	int sock;
	struct ifreq ifr;
	// Create a socket
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
	{
		eDebug("[Enigma] checkLinkStatus: Failed to create socket.");
		return 3;
	}
	// Set the interface name
	strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ);
	// Get the interface flags
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
	{
		eDebug("[Enigma] checkLinkStatus: Failed to get interface flags.");
		close(sock);
		return 3;
	}
	int ret = (ifr.ifr_flags & IFF_RUNNING) ? 0 : 3;
	close(sock);
	return ret;
}

#include <curl/curl.h>
#include <curl/easy.h>

size_t curl_ignore_output(void *ptr, size_t size, size_t nmemb, void *stream) // NOSONAR
{
	(void)ptr;
	(void)stream;
	return size * nmemb;
}

int checkInternetAccess(const char *host, int timeout = 3)
{

	int link = checkLinkStatus();
	if (link != 0)
	{
		eDebug("[Enigma] checkInternetAccess: No Active link.");
		return link;
	}

	CURL *curl;
	CURLcode res;
	int ret = 0; // SUCCESS
	curl = curl_easy_init();
	if (curl)
	{
		eDebug("[Enigma] checkInternetAccess: Check host:'%s' with timeout:%d.", host, timeout);
		curl_easy_setopt(curl, CURLOPT_URL, host);
		curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_ignore_output);
		while ((res = curl_easy_perform(curl)) != CURLE_OK)
		{
			switch (res)
			{
			case CURLE_COULDNT_RESOLVE_HOST:
				eDebug("[Enigma] checkInternetAccess: Failed to resolve host.");
				ret = 1;
				break;
			case CURLE_COULDNT_CONNECT:
			case CURLE_COULDNT_RESOLVE_PROXY:
				eDebug("[Enigma] checkInternetAccess: Failed.");
				ret = 2;
				break;
			default:
				eDebug("[Enigma] checkInternetAccess: Failed with error (%s).", curl_easy_strerror(res));
				ret = 2;
				break;
			}
			if (ret > 0)
				break;
		}
		curl_easy_cleanup(curl);
	}
	else
	{
		eDebug("[Enigma] checkInternetAccess: Failed to init curl.");
		return 2;
	}
	if (ret == 0)
		eDebug("[Enigma] checkInternetAccess: Success.");
	return ret;
}
