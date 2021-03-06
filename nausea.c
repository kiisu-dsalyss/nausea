#include <err.h>
#include <complex.h>
#include <curses.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>

#include <fftw3.h>

#define LEN(x) (sizeof (x) / sizeof *(x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#include "config.h"

static unsigned msec = 1000 / 25; /* fps */
static unsigned nsamples = 48000 * CHANNELS; /* stereo */
static wchar_t chbar = CHBAR;
static wchar_t chpeak = CHPEAK;
static wchar_t chpoint = CHPOINT;
static char *fname = "/tmp/audio.fifo";
static char *argv0;
static int colors;
static int peaks;
static int randompeaks;
static int monopeaks;
static int die;

struct frame {
	int fd;
	size_t width, width_old;
	size_t height;
	int *peak;
	int *sav;
	#define PK_HIDDEN -1
	int16_t *buf;
	unsigned *res;
	double *in;
	ssize_t gotsamples;
	complex *out;
	fftw_plan plan;
};

/* Supported visualizations:
 * 1 -- spectrum */
static void draw_spectrum(struct frame *fr);
static struct visual {
	void (* draw)(struct frame *fr);
	int dft;   /* needs the DFT */
	int color; /* supports colors */
} visuals[] = {
	{draw_spectrum, 1, 1},
};
static int vidx = 0; /* default visual index */
/* We assume the screen is 100 pixels in the y direction.
 * To follow the curses convention (0, 0) is in the top left
 * corner of the screen.  The `min' and `max' values correspond
 * to percentages.  To illustrate this the [0, 20) range gives
 * the top 20% of the screen to the color red.  These values
 * are scaled automatically in the draw() routine to the actual
 * size of the terminal window. */
static struct color_range {
	short pair; /* index in the color table */
	int min;    /* min % */
	int max;    /* max % */
	short fg;   /* foreground color */
	short bg;   /* background color */

	/* these are calculated internally, do not set */
	int scaled_min;
	int scaled_max;

  /* These are the colors that you can use:
   * COLOR_BLACK
   * COLOR_RED
   * COLOR_GREEN
   * COLOR_YELLOW
   * COLOR_BLUE
   * COLOR_MAGENTA
   * COLOR_CYAN
   * COLOR_WHITE  */
} color_ranges[] = {
	{ 1, 0,  5, 	COLOR_RED,		-1, 0, 0},
	{ 2, 5,  20,	COLOR_YELLOW, 	-1, 0, 0},
	{ 3, 20, 40, 	COLOR_GREEN, 	-1, 0, 0},
	{ 4, 40, 72,  	COLOR_WHITE,	-1, 0, 0},
	{ 5, 72, 85,  	COLOR_CYAN,		-1, 0, 0},
	{ 6, 85, 98,  	COLOR_BLUE,		-1, 0, 0},
	{ 7, 98, 100, 	COLOR_GREEN,	-1, 0, 0}
};

static void
clearall(struct frame *fr)
{
	unsigned i;

	fr->gotsamples = 0;
	for (i = 0; i < nsamples / CHANNELS; i++) {
		fr->in[i] = 0.;
		fr->out[i] = 0. + 0. * I;
	}
}

static void
init(struct frame *fr)
{
	fr->fd = open(fname, O_RDONLY | O_NONBLOCK);
	if (fr->fd == -1)
		err(1, "open %s", fname);

	fr->buf = malloc(nsamples * sizeof(int16_t));
	fr->res = malloc(nsamples / CHANNELS * sizeof(unsigned));
	fr->in = malloc(nsamples / CHANNELS * sizeof(double));
	fr->out = malloc(nsamples / CHANNELS * sizeof(complex));

	clearall(fr);

	fr->plan = fftw_plan_dft_r2c_1d(nsamples / CHANNELS, fr->in, fr->out,
					FFTW_ESTIMATE);
}

static void
done(struct frame *fr)
{
	fftw_destroy_plan(fr->plan);
	free(fr->out);
	free(fr->in);

	free(fr->res);
	free(fr->buf);
	free(fr->peak);
	free(fr->sav);

	close(fr->fd);
}

static void
update(struct frame *fr)
{
	ssize_t n;
	unsigned i;

	n = read(fr->fd, fr->buf, nsamples * sizeof(int16_t));
	if (n == -1) {
		clearall(fr);
		return;
	}

	fr->gotsamples = n / sizeof(int16_t);

	for (i = 0; i < nsamples / CHANNELS; i++) {
		fr->in[i] = 0.;
		if (i < fr->gotsamples / CHANNELS) {
			/* average the two CHANNELS */
			fr->in[i] = fr->buf[i * CHANNELS + 0];
			fr->in[i] += fr->buf[i * CHANNELS + 1];
			fr->in[i] /= CHANNELS;
		}
	}

	/* compute the DFT if needed */
	if (visuals[vidx].dft)
		fftw_execute(fr->plan);
}

static void
setcolor(int on, int y)
{
	unsigned i;
	struct color_range *cr;

	if (!colors)
		return;
	for (i = 0; i < LEN(color_ranges); i++) {
		cr = &color_ranges[i];
		if (y >= cr->scaled_min && y < cr->scaled_max) {
			if (on)
				attron(COLOR_PAIR(cr->pair));
			else
				attroff(COLOR_PAIR(cr->pair));
			return;
		}
	}
}

static void
draw_spectrum(struct frame *fr)
{
	unsigned i, j;
	unsigned freqs_per_col;
	struct color_range *cr;

	/* read dimensions to catch window resize */
	fr->width = COLS;
	fr->height = LINES;

	if (peaks) {
		/* change in width needs new peaks */
		if (fr->width != fr->width_old) {
			fr->peak = realloc(fr->peak, fr->width * sizeof(int));
			for (i = 0; i < fr->width; i++)
				fr->peak[i] = PK_HIDDEN;
			fr->width_old = fr->width;
		}
	}

	if (colors) {
		/* scale color ranges */
		for (i = 0; i < LEN(color_ranges); i++) {
			cr = &color_ranges[i];
			cr->scaled_min = cr->min * fr->height / 100;
			cr->scaled_max = cr->max * fr->height / 100;
		}
	}

	/* NARROW TO GUITAR FREQUENCY RANGE */
#define BANDCUT 0.03
        freqs_per_col = (nsamples / CHANNELS) / fr->width * BANDCUT;
#undef BANDCUT

	/* scale each frequency to screen */
#define BARSCALE 0.05
	for (i = 0; i < nsamples / 1; i++) {
		/* complex absolute value */
		fr->res[i] = cabs(fr->out[i]);
		/* normalize it */
		fr->res[i] /= (nsamples / CHANNELS);
		/* scale it */
		fr->res[i] *= fr->height * BARSCALE;
	}
#undef BARSCALE

	erase();
	attron(A_BOLD);
	for (i = 0; i < fr->width; i++) {
		size_t bar_height = 0;
		size_t ybegin, yend;

		/* compute bar height */
		for (j = 0; j < freqs_per_col; j++)
			bar_height += fr->res[i * freqs_per_col + j];
		bar_height /= freqs_per_col;

		/* we draw from top to bottom */
		ybegin = fr->height - MIN(bar_height, fr->height);
		yend = fr->height;

		/* If the current freq reaches the peak, the peak is
		 * updated to that height, else it drops by one line. */
		if (peaks) {
			if (fr->peak[i] >= ybegin)
				fr->peak[i] = ybegin;
			else
				fr->peak[i]++;
			/* this freq died out */
			if (fr->height == ybegin && fr->peak[i] == ybegin)
				fr->peak[i] = PK_HIDDEN;
		}

		/* output bars */
		for (j = ybegin; j < yend; j++) {
			move(j, i);
			setcolor(1, j);
			printw("%lc",chbar);
			setcolor(0, j);
		}

		/* output peaks */
		if (peaks && fr->peak[i] != PK_HIDDEN) {
			move(fr->peak[i], i);
			if(monopeaks) {
				setcolor(1, 1);
			}
			else 
				setcolor(1, fr->peak[i]);
			/* Make the peaks all matrix looking! */
			if (randompeaks) {
				char rndchar = 'A' + (rand() % (402+1-255))+255;
				printw("%lc", rndchar);
			}
			else
				printw("%lc",CHPEAK);
			setcolor(2, fr->peak[i]);
		}
	}
	attroff(A_BOLD);
	refresh();
}

static void
initcolors(void)
{
	unsigned i;
	struct color_range *cr;
	start_color();
	for (i = 0; i < LEN(color_ranges); i++) {
		cr = &color_ranges[i];
		init_pair(cr->pair, cr->fg, cr->bg);
	}
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-hcpklb] [-d num] [fifo]\n", argv0);
	fprintf(stderr, "default fifo path is `/tmp/audio.fifo'\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int c;
	struct frame fr;
	int vidx_prev;
	int fd;
	argv0 = argv[0];
	while (--argc > 0 && (*++argv)[0] == '-')
		while ((c = *++argv[0]))
			switch (c) {
			case 'd':
				if (*++argv == NULL)
					usage();
				argc--;
				switch (*argv[0]) {
				case '1':
					vidx = 0;
					break;
				}
				break;
			case 'c':
				colors = 1;
				break;
			case 'p':
				peaks = 1;
				break;
        	case 'm':
            	monopeaks = 1;
            	break;
            case 'r':
				randompeaks = 1;
				break;				
			case 'h':
				/* fall-through */
			default:
				usage();
			}
	if (argc == 1)
		fname = argv[0];
	else if (argc > 1)
		usage();

	/* init frame context */
	memset(&fr, 0, sizeof(fr));
	init(&fr);

	setlocale(LC_ALL, "");

	/* init curses */
	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	curs_set(FALSE); /* hide cursor */
	timeout(msec);
	use_default_colors();

	if (colors && has_colors() == FALSE) {
		endwin();
		done(&fr);
		errx(1, "your terminal does not support colors");
	}

	vidx_prev = vidx;

	while (!die) {
		switch (getch()) {
		case 'q':
			die = 1;
			break;
		case 'c':
			if (has_colors() == TRUE)
				colors = !colors;
			break;
		case 'p':
			peaks = !peaks;
			break;
        case 'r':
            randompeaks = !randompeaks;
            break;			
        case 'm':
            monopeaks = !monopeaks;
            break;
		case '1':
			vidx = 0;
			break;
		case 'n':
		case KEY_RIGHT:
			vidx = vidx == (LEN(visuals) - 1) ? 0 : vidx + 1;
			break;
		case 'N':
		case KEY_LEFT:
			vidx = vidx == 0 ? LEN(visuals) - 1 : vidx - 1;
			break;
		}

		/* detect visualization change */
		if (vidx != vidx_prev)
			fr.width_old = 0;

		/* only spectrum and fountain support colors */
		if (colors && visuals[vidx].color)
			initcolors();
		else
			(void)use_default_colors();

		update(&fr);
		visuals[vidx].draw(&fr);

		vidx_prev = vidx;
	}

	endwin(); /* restore terminal */

	done(&fr); /* destroy context */

	return (0);
}
