/*
 * space invaders game
 *
 * works on OSX. was told this doesn't have to work on Windows
 * so it doesn't. if you want to run this on windows, link against
 * GLUT and implement read_jpeg_image/file_exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>
#include <map>
#include <initializer_list>
#include <ostream>
#include <fstream>

#include <OpenGL/OpenGL.h>
#include <GLUT/GLUT.h>

#include <atomic>

/***************************************************************
 * TYPES, GLOBALS AND CONSTANTS FOR THE GAME
 ***************************************************************/

class game_t;
static game_t* gGame;

static int gDifficultyLevels[][2]  = {
	/* { speed, number_of_cols } */
	{ 2, 6 },
	{ 3, 6 },
	{ 2, 8 },
	{ 3, 8 }
};

#define MAX_LEVEL 3

/* lol raii */
class gl_transaction_t {
public:
	gl_transaction_t(GLint t) {
		glBegin(t);
	}
	~gl_transaction_t() {
		glEnd();
	}
};


/* texture IDs */
enum texture_t {
	kTexDestroyer = 0,
	kTexBullet = 1,
	kTexMothership = 2,
	kTexMartian = 3,
	kTexMeteor = 4,
	kTexPlayer = 5,
	kTexVenusian = 6,
	kTexMercurian = 7,
	_kTexEnd = 8
};

/* 2d position vector */
struct pt_t {
	float x, y;
};
/* rectangle */
struct rect_t {
	pt_t pt;
	float w, h;
};

#define PLAYER_WIDTH 30.0f
#define PLAYER_HEIGHT 20.0f

#define PROJ_WIDTH 2.0f
#define PROJ_HEIGHT 12.0f

#define DIRECTION_RIGHT_TO_LEFT 0
#define DIRECTION_LEFT_TO_RIGHT 1

#define STATE_PLAYING 0x1
#define STATE_WON 0x2
#define STATE_LOST 0x4
#define STATE_MOTHERSHIP 0x10
#define STATE_RESUME 0x20

#define AXIS_X 0
#define AXIS_Y 1

#define SAVEDATA_MAGIC 0xFEEDFEED
#define SAVEDATA_FILE "savedata.bin"
#define HIGHSCORE_FILE "highscore.bin"

#define medium_probability() ((rand() % 500) == 1)
#define low_probability() ((rand() % 2000) == 1)
#define high_probability() ((rand() % 100) == 1)

/***************************************************************
 * UTILS & GLOBALS
 ***************************************************************/

/* need jpeg loading */
#if __APPLE__
/* link against cg, cf and imageio */
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
const uint8_t* read_jpeg_image (const char* path, size_t& len, GLfloat& w, GLfloat& h) {
	auto cfstr = CFStringCreateWithCStringNoCopy(NULL, path, kCFStringEncodingUTF8, NULL);
	auto url = CFURLCreateWithFileSystemPath(NULL, cfstr, kCFURLPOSIXPathStyle, FALSE);
	auto imgsrc = CGImageSourceCreateWithURL(url, NULL);
	assert(imgsrc);
	auto img = CGImageSourceCreateImageAtIndex(imgsrc, 0, NULL);
	auto data = CGDataProviderCopyData(CGImageGetDataProvider(img));
	auto rawdata = CFDataGetBytePtr(data);
	
	/* get stuff */
	h = CGImageGetHeight(img);
	w = CGImageGetWidth(img);
	len = CFDataGetLength(data);
	
	/* free up resources */
	CFRelease(img);
	CFRelease(imgsrc);
	CFRelease(url);
	//CFRelease(data);
	
	// CFRelease(cfstr); // leak this because of allocator stupidity :(
	
	/* make sure to free after use, unique_ptr is too 
	   annoying to use uhh */
	return static_cast<const uint8_t*>(rawdata);
}
bool file_exists(const char* path) {
	return access(path, O_RDONLY) == 0;
}
#elif __WINDOWS__
#error add win32 support
#else
#error use a normal os lol
#endif

/***************************************************************
 * BINARY STREAM FOR SERIALIZATION
 ***************************************************************/

class binary_stream {
	std::fstream stream;
	
#define MAKE_IO(T)\
	binary_stream& operator>> (T& v) {\
		T b;\
		stream.read(reinterpret_cast<char*>(&b), sizeof(b));\
		v = b;\
		return *this;\
	}\
	binary_stream& operator<< (const T& v) {\
		stream.write(reinterpret_cast<const char*>(&v), sizeof(v));\
		return *this;\
	}

public:
	binary_stream(const char* path, bool write) :
	stream(path, (write ? (std::fstream::trunc | std::fstream::out) : std::fstream::in) | std::fstream::binary) {
		
	}
	
	MAKE_IO(signed int)
	MAKE_IO(unsigned int)
	MAKE_IO(bool)
	MAKE_IO(float)
	
	MAKE_IO(texture_t)
};

/***************************************************************
 * GL RENDERER
 ***************************************************************/

class renderer_t {
public:
	GLfloat surface_w, surface_h;
	
	/* create a GPU texture from an RGBA bitmap */
	GLuint load_texture(const char* name) {
		size_t len;
		GLfloat w, h;
		
		/* argghhhh loading jpeg files is annoying */
		auto p = read_jpeg_image(name, len, w, h);
		
		GLuint texid;
		
		glGenTextures(1, &texid);
		glBindTexture(GL_TEXTURE_2D, texid);
		
		/* i really hope the pixel format matches */
		gluBuild2DMipmaps(GL_TEXTURE_2D, 4, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);

		return texid;
	}
	
	/* init OpenGL state */
	void init_state() {
		/* we're doing 2d drawing so we don't need depth buffering */
		glDisable(GL_DEPTH_TEST);
		
		/* set up ortho projection for 2d drawing */
		glViewport(0, 0, surface_w, surface_h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		gluOrtho2D(0, surface_w, surface_h, 0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	
	/* draw a string */
	void draw_string(GLfloat x, GLfloat y, const char* s, float r=1, float g=1, float b=1) {
		glDisable(GL_TEXTURE_2D);
		glColor3f(r, g, b);
		
		glRasterPos3f(x, 15 + y, 0);
		
		/* yes glut font rendering is slow but doing it better
		 * requires too much effort
		 */
		for (; *s != '\0'; s++)
			glutBitmapCharacter(GLUT_BITMAP_9_BY_15, *s);
	}
	
	void draw_stringm(GLfloat y, const char* s, float r=1, float g=1, float b=1) {
		GLfloat mid = (surface_w / 2) - ((GLfloat)(strlen(s) * 9) / 2);
		draw_string(mid, y, s, r, g, b);
	}
	
	void fill_quad(GLfloat x, GLfloat y, GLfloat w, GLfloat h, bool textured=true, float r=1, float g=1, float b=1, bool blend=true) {
		glColor4f(r, g, b, 1.0f);
		if (!textured) glDisable(GL_TEXTURE_2D);
		
		if (blend) glEnable(GL_BLEND);
		else glDisable(GL_BLEND);
		
		/*
		 * draw the quad. if we're using a texture, remap the texture to
		 * the quad's relative coordinate space.
		 */
		{
			gl_transaction_t t(GL_QUADS);
			
			if (textured)
				glTexCoord2f(1, 0);
			glVertex2f(x, y);
			
			if (textured)
				glTexCoord2f(0, 0);
			glVertex2f(x + w, y);
			
			if (textured)
				glTexCoord2f(0, 1);
			glVertex2f(x + w, y + h);
			
			if (textured)
				glTexCoord2f(1, 1);
			glVertex2f(x, y + h);
		}
	}
	
	/* select a preloaded GPU texture */
	void bind_tex(GLuint tex) {
		glEnable(GL_TEXTURE_2D);
		
		/* enable texture filtering so they don't look like ass */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

		glBindTexture(GL_TEXTURE_2D, tex);
	}
	
	void clear() {
		glClear(GL_COLOR_BUFFER_BIT);
	}
};

/***************************************************************
 * PROJECTILES
 ***************************************************************/

struct projectile_t {
	float x, y;
	
	/*
	 * collision detection:
	 *   test this projectile against a rect
	 */
	bool test(rect_t rect) {
		return (this->x <= (rect.pt.x + rect.w) &&
				rect.pt.x <= (this->x + PROJ_WIDTH) &&
				this->y <= (rect.pt.y + rect.h) &&
				rect.pt.y <= (this->y + PROJ_HEIGHT));
	}
	
	void deact() {
		y = -1;
	}
};

/***************************************************************
 * ABSTRACT ENEMIES
 ***************************************************************/


/*
 * c++ doesn't have abstracts per se, if a class has at least one
 * pure virtual fn, it's "abstract".
 */
class enemy_t {
protected:
	/*
	 * texture ID, used as an "image" for drawing the thing (each
	 * texture ID corresponds to a loaded image mapped out in the
	 * game class)
	 */
	texture_t tex;
	int points;
	
	/* pure virtuals */
	virtual pt_t get_pt() = 0;
	
	enemy_t(texture_t t, int pts) {
		tex = t;
		points = pts;
		visible = false;
		active = false;
		w = 30; h = 20;
	}
	
public:
	bool active;
	bool visible;
	float w, h;

	virtual ~enemy_t() {}
	
	virtual void act() {
		active = visible = true;
	}
	
	void deact() {
		visible = false;
		active = false;
	}
	
	int die() {
		deact();
		return points;
	}
	
	bool is_visible() {
		return visible;
	}

	texture_t get_texture_id() {
		return tex;
	}
	
	virtual void marshal(binary_stream& s) {
		s << tex << points << visible << active << w << h;
	}
	
	virtual void unmarshal(binary_stream& s) {
		/* omit tex as that's used to construct the object */
		s >> points >> visible >> active >> w >> h;
	}
	
	/* defined after all the enemies */
	static enemy_t* unmarshal_base(binary_stream& s, texture_t t);
};

/* independent enemy (moves on its own) */
class e_independent_t : public enemy_t {
	int axis;
	
protected:
	int dir;
	int max_lives;
	float speed_factor;
	
public:
	pt_t pos;
	int lives;
	
	virtual pt_t get_pt() override {
		return pos;
	}

	virtual bool bounce() {
		return false;
	}
	
	/*
	 * advance a single, standalone enemy. this can be implemented
	 * here as it relies on very little global state unlike anchored
	 * aliens. returns if we updated the scene or not.
	 */
	bool advance(int speed, float g_w, float g_h) {
		/* constaints and reference for the update */
		float& u_pt    = axis == AXIS_X ? pos.x : pos.y;
		float  u_bound = axis == AXIS_X ? g_w : g_h;
		float  u_size  = axis == AXIS_X ? w : h;
		
		speed *= speed_factor;
		
		if (dir == DIRECTION_RIGHT_TO_LEFT)
			speed = -speed;
		
		/*
		 * check that we fall within the constraint. if we do,
		 * move us, if we don't, call bounce and let the subclass
		 * handle it.
		 */
		if (((u_pt+speed < 0) || (u_pt+speed+u_size > u_bound)) && bounce()) {
			return false;
		}
		else {
			/* advance by speed */
			u_pt += speed;
			return true;
		}
	}
	
	/*
	 * test a (player's) projectile against this enemy.
	 * returns: positive value if died, 0 if miss, -1 if hit but life lost.
	 */
	int collide(projectile_t& proj) {
		/* make sure the projectile is active */
		if (proj.y <= 0)
			return 0;
		
		/* create rect representing this enemy */
		rect_t r = {pos, w, h};
		
		/* perform collision detection against the projectile */
		if (proj.test(r)) {
			/*
			 * we've been hit. if we're now at 0 lives, die, otherwise
			 * do nothing and return 0 points.
			 */
			proj.deact();
			if(!--lives) {
				die();
				return points;
			}
		}
		return 0;
	}
	
	virtual void act() override {
		enemy_t::act();
		lives = max_lives;
	}
	
	virtual void marshal(binary_stream& s) override {
		enemy_t::marshal(s);
		s << axis << dir << speed_factor << lives << max_lives << pos.x << pos.y;
	}
	
	virtual void unmarshal(binary_stream& s) override {
		enemy_t::unmarshal(s);
		s >> axis >> dir >> speed_factor >> lives >> max_lives >> pos.x >> pos.y;
	}
	
protected:
	e_independent_t(texture_t t, int pts, int a) : enemy_t(t, pts) {
		axis = a;
		dir = DIRECTION_LEFT_TO_RIGHT;
		max_lives = 1;
		speed_factor = 1;
	}
};

/* anchored enemy (moves relative to others) */
class e_anchored_t : public enemy_t {
public:
	int grid_row, grid_col;
	
	virtual pt_t get_pt() override {
		return {
			grid_col * w + (grid_col * 10) /* spacing */,
			grid_row * h
		};
	}
	
	virtual void marshal(binary_stream& s) override {
		enemy_t::marshal(s);
		s << grid_col << grid_row;
	}
	
	virtual void unmarshal(binary_stream& s) override {
		enemy_t::unmarshal(s);
		s >> grid_col >> grid_row;
	}
	
	virtual ~e_anchored_t() {}
	
protected:
	e_anchored_t(texture_t t, int pts) : enemy_t(t, pts) {}
};

/*
 * interfaces (not really, but c++ has multiple inheritance)
 */

/* thing can fire */
class e_fireable_t {

	/* RTTI */
public:
	virtual ~e_fireable_t() {}
};
/* thing can cloak */
class e_cloakable_t {

	/* RTTI */
public:
	virtual ~e_cloakable_t() {}
};

/***************************************************************
 * CONCRETE ENEMIES
 ***************************************************************/

/* can fire back at the player. 30 points. */
class e_martian_t : public e_anchored_t, public virtual e_fireable_t {
public:
	e_martian_t() : e_anchored_t(kTexMartian, 30) {}
	virtual ~e_martian_t() {}
};

/* can cloak at random times. 40 points. */
class e_mercurian_t : public e_anchored_t, public virtual e_cloakable_t {
public:
	e_mercurian_t() : e_anchored_t(kTexMercurian, 40) {}
	virtual ~e_mercurian_t() {}
};

/* 20 points. */
class e_venusian_t : public e_anchored_t {
public:
	e_venusian_t() : e_anchored_t(kTexVenusian, 20) {}
};

/* */
class e_destroyer_t : public e_independent_t, public e_cloakable_t {
public:
	e_destroyer_t() : e_independent_t(kTexDestroyer, 200, AXIS_X) {
		w = 50;
		h = 34;
		max_lives = 2;
		speed_factor = 0.5;
	}
};

/* */
class e_mothership_t : public e_independent_t, public e_fireable_t {
public:
	e_mothership_t() : e_independent_t(kTexMothership, 100, AXIS_X) {
		max_lives = 3;
		speed_factor = 0.5;
		w = 50;
		h = 34;
	}
	
	/*
	 * mothership's direction reverses and speed increases
	 * on bounce.
	 */
	virtual bool bounce() override {
		dir = !dir;
		speed_factor += 0.3;
		pos.y += 20;
		return true;
	}
};

/* */
class e_meteor_t : public e_independent_t {
public:
	e_meteor_t() : e_independent_t(kTexMeteor, 100, AXIS_Y) {
		w = 40;
		h = 40;
	}
};

/* player has to be a class (i think?) */
class player_t {
public:
	/* player position vector */
	projectile_t proj;
	
	/* projectile */
	pt_t pt;
};

/***************************************************************
 * SERIALIZATION
 ***************************************************************/

/*
 * construct enemy base class from the tex property
 */
enemy_t* enemy_t::unmarshal_base(binary_stream& s, texture_t t) {
	enemy_t* e;
	
#define Map(x,y) case x: e = new y(); break;
	switch (t) {
		Map(kTexMartian, e_martian_t)
		Map(kTexVenusian, e_venusian_t)
		Map(kTexMercurian, e_mercurian_t)
		default: abort();
	}
#undef Map
	
	return e;
}

/***************************************************************
 * GAME GUTS
 ***************************************************************/

/* game class */
class game_t {
private:
	
	int speed,
		columns,
		lives,
		points,
		state,
		level,
		frame,
		movement_dir,
		enemy_count,
		highscore,
		player_delta;
	
	unsigned long timebase, time;
	
	/* base vector of the enemy grid */
	pt_t enemy_anchor;

	/* player instance */
	player_t player;
	
	/* enemies array */
	std::vector<e_anchored_t*> anchored_enemies;
	
	/* we don't keep track of who fired the projectile since 
	 
	 */
	std::vector<projectile_t> enemy_projectiles;
	
	/*
	 * special enemies (can only have one on screen at a
	 * given time so having more here would make no sense.
	 */
	e_mothership_t enemy_mothership;
	e_destroyer_t enemy_destroyer;
	e_meteor_t enemy_meteor;
	
	/* gl surface/renderer */
	renderer_t rend;
	
	/* texture array */
	GLuint textures[_kTexEnd];
	
	/* calc midx of player sprite */
	inline GLfloat player_midx() {
		return (player.pt.x) + (PLAYER_WIDTH / 2.0f);
	}
	
	/* schedule the next timer tick */
	inline void resched() {
		glutTimerFunc(2, __glut_timer_fn, 0);
	}
	
	void start_mothership() {
		state = STATE_PLAYING | STATE_MOTHERSHIP;
		
		enemy_count++;
		enemy_mothership.pos.x = 0;
		enemy_mothership.pos.y = enemy_mothership.h;
		enemy_mothership.act();
	}
	
	/*
	 * gets called when enemy counter hits 0. either enter
	 * mothership stage or genuinely win (after mothership)
	 */
	void win() {
		save_highscore();
		
		if (state & STATE_MOTHERSHIP) {
			state = STATE_WON;
		}
		else {
			start_mothership();
		}
	}
	
	void lose() {
		save_highscore();
		state = STATE_LOST;
	}
	
	void reset_if_possible() {
		if (state & STATE_PLAYING)
			return;
		else if (state & STATE_WON) {
			if (level < MAX_LEVEL-1) {
				load_level(level+1);
			}
		}
		reset();
	}
	
	inline bool advance_if_active(e_independent_t& e) {
		if (!e.active)
			return false;
		
		return e.advance(speed, rend.surface_w, rend.surface_h);
	}
	
	void on_enemy_hit(int sc) {
		points += sc;
		enemy_count--;
		
		if (points > highscore) {
			highscore = points;
		}
	}
	
	void on_player_hit() {
		lives--;
		
		if (!lives) lose();
	}
	
	/*
	 * serialize/unserialize
	 *
	 * prefixed by a fixed blob of global game data followed by serialized
	 * enemies identified by their texture id.
	 */
	void unmarshal(binary_stream& s) {
		/*
		 * nops = number of enemies following the game data
		 */
		int magic, nops;
		texture_t op;
		
		s >> magic;
		assert(magic == SAVEDATA_MAGIC);
		
		s >> columns >> speed >> lives >> points >> movement_dir >> enemy_count
		  >> enemy_anchor.y >> enemy_anchor.x >> state >> nops;
		
		/* player */
		s >> player.pt.y >> player.pt.x;
		
		/*
		 * unmarshal enemies
		 */
		while (nops--) {
			/* read opcode */
			s >> op;
			switch(op) {
				case kTexDestroyer:
					enemy_destroyer.unmarshal(s);
					break;
				case kTexMeteor:
					enemy_meteor.unmarshal(s);
					break;
				case kTexMothership:
					enemy_mothership.unmarshal(s);
					break;
				default:
					/*
					 * unmarshal dynamically allocated enemy
					 */
					e_anchored_t* e = static_cast<e_anchored_t*>(enemy_t::unmarshal_base(s, op));
					e->unmarshal(s);
					anchored_enemies.push_back(e);
					break;
			}
		}
	}
	
	void marshal(binary_stream& s) {
		int nops = static_cast<int>(anchored_enemies.size()) + 3;
		
		s << SAVEDATA_MAGIC;
		
		s << columns << speed << lives << points << movement_dir << enemy_count
		  << enemy_anchor.y << enemy_anchor.x << state << nops;
		
		/* player */
		s << player.pt.y << player.pt.x;
	
		enemy_meteor.marshal(s);
		enemy_mothership.marshal(s);
		enemy_destroyer.marshal(s);
		
		for (e_anchored_t* e : anchored_enemies)
			e->marshal(s);
	}
	
	void process_enemy_fire(e_fireable_t& ee, enemy_t& e, rect_t r) {
		/*
		 * during the mothership stage, mothership should fire with
		 * a high probability.
		 */
		if (e.active && (state & STATE_MOTHERSHIP ? high_probability() : low_probability())) {
			projectile_t p = {
				r.pt.x + (r.w / 2),
				r.pt.y + r.h
			};
			enemy_projectiles.push_back(p);
		}
	}
	
	void process_enemy_fire(e_fireable_t& ee, e_independent_t& e) {
		process_enemy_fire(ee, e, {e.pos, e.w, e.h});
	}
	
	void process_enemy_cloak(e_cloakable_t& ee, enemy_t& e) {
		if (e.active && medium_probability()) {
			e.visible = !e.visible;
		}
	}
	
	/*
	 * fixed rate tick function that is responsible for most
	 * timed state updates within the game.
	 */
	void tick() {
#define MARKS state_changed = true;
		bool state_changed = false;
		
		/* move player within screen bounds */
		if ((player.pt.x+player_delta) >= 0 && (player.pt.x+player_delta) < (rend.surface_w-PLAYER_WIDTH))
			player.pt.x += player_delta;
		
		/*
		 * this is done in a more convoluted way than the case
		 * with a single enemy since we need to calculate sizes
		 * for the enemy grid.
		 */
		if (movement_dir == DIRECTION_LEFT_TO_RIGHT) {
			GLfloat rightmost = calc_rightmost();
			
			if (rend.surface_w < (rightmost + speed))
				movement_dir = DIRECTION_RIGHT_TO_LEFT;
			else {
				enemy_anchor.x += speed;
				MARKS
			}
		}
		else {
			if (calc_leftmost()	< (0+speed))
				movement_dir = DIRECTION_LEFT_TO_RIGHT;
			else {
				enemy_anchor.x -= speed;
				MARKS
			}
		}
		
		/* avoid over/underdraw */
		if (!state_changed) {
			enemy_anchor.y += 20;
		}
		
		/* advance unique enemies */
		state_changed = advance_if_active(enemy_mothership) || state_changed;
		state_changed = advance_if_active(enemy_destroyer) || state_changed;
		
		/*
		 * if the meteor reached player's line, lose a life
		 */
		state_changed = advance_if_active(enemy_meteor) || state_changed;
		
		/* advance player's single projectile if needed */
		if (player.proj.y > 0) {
			/* hit-test */
			for (e_anchored_t* ep : anchored_enemies) {
				e_anchored_t& e = *ep;
				
				/* get an absolute rect for the enemy */
				rect_t rect = { anchored_vec(e), e.w, e.h };
				
				if (e.active && player.proj.test(rect)) {
					/* collision, deal with the enemy */
					on_enemy_hit(e.die());

					/* get rid of the projectile */
					player.proj.deact();
				}
			}
			
			player.proj.y -= 12;
			MARKS
		}
		
		if (enemy_count == 0) {
			win();
			MARKS
		}
		
		/*
		 * if invaders reached player, it's an instant game loss
		 */
		else if (calc_bottommost() >= player.pt.y) {
			lose();
			MARKS
		}
		
		/* introduce and handle enemies that appear by chance */
		if (state & STATE_PLAYING) {
			/* only introduce them during the main fight phase */
			if ((state & STATE_MOTHERSHIP) == 0) {
				/*
				 * meteor: start at a random Y. if it reaches
				 * bottom of the screen, lose a life.
				 */
				if (!enemy_meteor.active) {
					if (medium_probability()) {
						enemy_count++;
						enemy_meteor.pos.y = 0;
						enemy_meteor.pos.x = (float)(rand() %
													 (int)(rend.surface_w - enemy_meteor.w));
						enemy_meteor.act();
					}
				}
				else if (enemy_meteor.pos.y+enemy_meteor.h > player.pt.y) {
					enemy_count--;
					enemy_meteor.deact();
					on_player_hit();
				}
				else {
					int sc = enemy_meteor.collide(player.proj);
					if (sc) on_enemy_hit(sc);
				}
				
				/*
				 * destroyer
				 */
				if (!enemy_destroyer.active) {
					if (medium_probability()) {
						enemy_count++;
						enemy_destroyer.pos.y = 10;
						enemy_destroyer.pos.x = 0;
						enemy_destroyer.act();
					}
				}
				else if (enemy_destroyer.pos.x > rend.surface_w) {
					enemy_count--;
					enemy_destroyer.deact();
				}
				else {
					int sc = enemy_destroyer.collide(player.proj);
					if (sc) on_enemy_hit(sc);
				}
			}
			else {
				/* mothership */
				int sc = enemy_mothership.collide(player.proj);
				if (sc) on_enemy_hit(sc);
			}
			
			/* cloaked/fireable enemies */
			for (e_anchored_t* e : anchored_enemies) {
				{
					e_fireable_t* ee = dynamic_cast<e_fireable_t*>(e);
					if (ee) process_enemy_fire(*ee, *e, { anchored_vec(*e), e->w, e->h });
				}
				{
					e_cloakable_t* ee = dynamic_cast<e_cloakable_t*>(e);
					if (ee) process_enemy_cloak(*ee, *e);
				}
			}
			
			process_enemy_fire(enemy_mothership, enemy_mothership);
			process_enemy_cloak(enemy_destroyer, enemy_destroyer);
			
			/* advance enemy projectiles */
			std::vector<projectile_t>::iterator it = enemy_projectiles.begin();
			for (; it != enemy_projectiles.end(); ) {
				projectile_t& p = *it;
				
				/* did we hit a player */
				if (p.test({player.pt, PLAYER_WIDTH, PLAYER_HEIGHT})) {
					on_player_hit();
					enemy_projectiles.erase(it);
				}
				/* did we go off screen */
				else if (p.y > rend.surface_h) {
					enemy_projectiles.erase(it);
				}
				else {
					p.y += 7;
					it++;
				}
			}
			
			/* handle fireables and cloakables */
			MARKS
		}
		
		if (state_changed)
			glutPostRedisplay();
		
		if (state & STATE_PLAYING)
			resched();
#undef MARKS
	}
	
	/* calculate rightmost active  x for enemy grid */
	GLfloat calc_rightmost() {
		GLfloat r = 0;
		for (e_anchored_t* e : anchored_enemies)
			if (e->active)
				r = MAX(r, e->get_pt().x + e->w);
		return r + enemy_anchor.x;
	}
	
	/* leftmost active x */
	GLfloat calc_leftmost() {
		GLfloat r = rend.surface_h;
		for (e_anchored_t* e : anchored_enemies)
			if (e->active)
				r = MIN(r, e->get_pt().x);
		return r + enemy_anchor.x;
	}
	
	/* bottommost active y */
	GLfloat calc_bottommost() {
		GLfloat r = 0;
		for (e_anchored_t* e : anchored_enemies)
			if (e->active)
				r = MAX(r, e->get_pt().y + e->h);
		return r + enemy_anchor.y;
	}
	
	void save_highscore() {
		if (points < highscore)
			return;
		
		binary_stream s(HIGHSCORE_FILE, true);
		s << points;
		
		highscore = points;
	}
	
	void load_highscore() {
		if (!file_exists(HIGHSCORE_FILE)) {
			highscore = 0;
		}
		else {
			binary_stream s(HIGHSCORE_FILE, false);
			s >> highscore;
		}
	}
	
	void save_game() {
		save_highscore();
		
		binary_stream s(SAVEDATA_FILE, true);

		/* marshal the program state */
		marshal(s);
	}
	
	void load_game() {
		binary_stream s(SAVEDATA_FILE, false);
		
		clear_enemies();
		unmarshal(s);
		
		/*
		 * this is sort of like reset() except with
		 * savedata values.
		 */
		resched();
	}
	
	bool has_savegame_file() {
		return file_exists(SAVEDATA_FILE);
	}
	
	void scan_key(unsigned char k, bool down) {
		if (!down) return;
		
		switch(k)
		{
			case '\r':
				reset_if_possible();
				break;
			case 27: /* esc key */
				save_game();
				exit(0);
				break;
			case ' ':
				player_fire();
				break;
			case 's':
				if (state == STATE_RESUME) load_game();
				break;
		}
	}
	
	void scan_key(int k, bool down) {
		switch(k)
		{
			case GLUT_KEY_LEFT:
				if (down)
					player_delta = -4;
				else
					player_delta = 0;
				break;
			case GLUT_KEY_RIGHT:
				if (down)
					player_delta = 4;
				else
					player_delta = 0;
				break;
			case GLUT_KEY_UP:
				if (down)
					player_fire();
				break;
		}
	}
	
	void player_fire() {
		player.proj.y = player.pt.y - PLAYER_HEIGHT;
		player.proj.x = player_midx() - 1;
	}
	
	/* bind mapped texture by ID */
	inline void bmap_tex(texture_t t) {
		rend.bind_tex(textures[t]);
	}
	
	/* map anchored enemy to absoulute coords */
	inline pt_t anchored_vec(e_anchored_t& e) {
		pt_t rela = e.get_pt();
		return {
			enemy_anchor.x + rela.x,
			enemy_anchor.y + rela.y
		};
	}
	
	void draw_independent_enemy(e_independent_t& e) {
		if (!e.is_visible())
			return;
		
		bmap_tex(e.get_texture_id());
		rend.fill_quad(e.get_pt().x, e.get_pt().y, e.w, e.h, true, 1, 1, 1, false);
	}
	
	/*
	 * this function is responsible for redrawing the whole scene every frame.
	 */
	void display() {
		/* status string buffer */
		char fmtbuf[128];
		snprintf(fmtbuf, sizeof(fmtbuf), "Level: %d Lives: %d Score: %d Highscore: %d", level+1, lives, points, highscore);
		
		rend.clear();
		
		if (state & STATE_PLAYING) {
			/* draw player sprite */
			bmap_tex(kTexPlayer);
			rend.fill_quad(player.pt.x, player.pt.y, PLAYER_WIDTH, PLAYER_HEIGHT);
			
			if (player.proj.y > 0) {
				/* draw player projective if needed */
				rend.fill_quad(player.proj.x, player.proj.y, 2, 12, false, 0, 1, 0);
			}
			
			if (state & STATE_MOTHERSHIP)
				draw_independent_enemy(enemy_mothership);
			else {
				draw_independent_enemy(enemy_destroyer);
				draw_independent_enemy(enemy_meteor);
				
				/* draw enemies (yay c++11 iterators) */
				for (e_anchored_t* e : anchored_enemies) {
					if (!e->is_visible())
						continue;
					
					pt_t abs = anchored_vec(*e);
					
					/* bind preselected texture and draw */
					bmap_tex(e->get_texture_id());
					rend.fill_quad(abs.x, abs.y, e->w, e->h);
				}
			}

			for (projectile_t& p : enemy_projectiles) {
				rend.fill_quad(p.x, p.y, 2, 12, false, 1, 0, 0);
			}
		}
		
		/* draw status string on top */
		rend.draw_string(0, 0, fmtbuf);
	
		/* win lose notification strings */

		if (state & STATE_RESUME) {
			if (has_savegame_file()) {
				rend.draw_stringm(220,
								  "Press 'Enter' to start or 's' to load saved game!");
			}
			else {
				rend.draw_stringm(220,
								  "Press 'Enter' to start!");
			}
		}
		else if (state & STATE_WON) {
			rend.draw_stringm(200,
							 "Well done, you won!",
							 0, 1, 0);
			
			if (level != MAX_LEVEL)
				rend.draw_stringm(220,
								  "Press 'Enter' to go to next level!");
			else
				rend.draw_stringm(220,
								  "Press 'Enter' to try again!");
		}
		else if (state & STATE_LOST) {
			rend.draw_stringm(200,
							 "You lost, too bad!",
							 1, 0, 0);
			rend.draw_stringm(220,
							  "Press 'Enter' to try again!");
		}
		else if (state & STATE_MOTHERSHIP) {
			snprintf(fmtbuf, sizeof(fmtbuf), "Mothership: %d Lives Left", enemy_mothership.lives);
			rend.draw_string(0, 16, fmtbuf, 1, 1, 0);
		}
		
		frame++;
		time = glutGet(GLUT_ELAPSED_TIME);
		
		if (time - timebase > 1000) {
			unsigned long fps = frame*1000.0/(time-timebase);
			timebase = time;
			frame = 0;
			
			//printf("FPS: %lu\n", fps);
		}
		
		/* commit buffer */
		glutSwapBuffers();
	}
	
	void load_textures() {
		/*
		 * some ugly macros and code to populate the
		 * tex array for later.
		 */
#define T(k, p) textures[k] = rend.load_texture("images/" p ".png");
		T(kTexDestroyer, "destroyer");
		
		T(kTexMothership, "mothership");
		T(kTexMartian, "martian");
		T(kTexMeteor, "meteor");
		T(kTexPlayer, "Space-invaders");
		T(kTexVenusian, "venusian");
		T(kTexMercurian, "mercurian");
#undef T
	}
	
	/*
	 * glut callbacks dispatch - we need static functions so we can take
	 * their pointers and pass them to glut. these function act as trampolines
	 * and pass events back to the game.
	 */
	static void __glut_display_fn() {
		gGame->display();
	}
	static void __glut_timer_fn(int t) {
		gGame->tick();
	}
	static void __glut_kbd_fn(int k, int x, int y) {
		gGame->scan_key(k, true);
	}
	static void __glut_kbdup_fn(int k, int x, int y) {
		gGame->scan_key(k, false);
	}
	static void __glut_kbd_fn_char(unsigned char k, int x, int y) {
		gGame->scan_key(k, true);
	}
	static void __glut_kbdup_fn_char(unsigned char k, int x, int y) {
		gGame->scan_key(k, false);
	}
						
	/* create glut window */
	void init_glut_win() {
		glutInitDisplayMode(GLUT_RGBA | GLUT_SINGLE);
		glutInitWindowSize(rend.surface_w, rend.surface_h);
		glutCreateWindow("Space Invaders");
		
		/* static callback because glut is retarded */
		
		glutDisplayFunc(&__glut_display_fn);
		glutSpecialFunc(&__glut_kbd_fn);
		glutSpecialUpFunc(&__glut_kbdup_fn);
		glutKeyboardFunc(&__glut_kbd_fn_char);
		glutKeyboardUpFunc(&__glut_kbdup_fn_char);
		
		rend.init_state();
		
		load_textures();
	}
	
	/* function template to create grid enemies */
	template <typename T>
	void create_grid_alien(int c, int r) {
		T* e = new T();
		e->grid_col = c;
		e->grid_row = r;
		
		/* add enemy to array */
		anchored_enemies.push_back(e);
		enemy_count++;
	}
	
	/*
	 * create a set of enemies for each colum.
	 */
	void create_enemies() {
		/* populate columns */
		for (int i = 0; i < columns; i++) {
			/* populate three rows with different enemy type for each row */
			create_grid_alien<e_martian_t>(i, 0);
			create_grid_alien<e_mercurian_t>(i, 1);
			create_grid_alien<e_venusian_t>(i, 2);
		}
	}
	
	void clear_enemies() {
		for (e_anchored_t* e : anchored_enemies)
			delete e;
		anchored_enemies.clear();
	}
	
	/* fully reset game state */
	void reset() {
		/* activate all enemies */
		for (e_anchored_t* e : anchored_enemies)
			e->act();
		
		enemy_projectiles.clear();
		
		/* at reset player is in the middle */
		player.pt.x = (rend.surface_w / 2.0f) - (PLAYER_WIDTH / 2.0f);
		player.pt.y = rend.surface_h - 50.0f;
		
		/* reset enemy positions */
		enemy_anchor.x = 0;
		enemy_anchor.y = 30;
		
		player.proj.y = -1;
		
		/* reset score and lives */
		points = 0;
		lives = 3;
		
		/* set initial enemy count */
		enemy_count = static_cast<int>(anchored_enemies.size());
		state = STATE_PLAYING;
		
		movement_dir = DIRECTION_LEFT_TO_RIGHT;

		/* schedule timer */
		resched();
	}
	
	/* load level data and populate enemies */
	void load_level(int l) {
		level = l;
		
		clear_enemies();
		
		/* load level info */
		speed = gDifficultyLevels[level][0];
		columns = gDifficultyLevels[level][1];
		
		create_enemies();
	}
	
public:
	void init() {
		/* surface size */
		rend.surface_h = 500;
		rend.surface_w = 600;
		player_delta = 0;
		
		load_highscore();
		load_level(0);
		reset();
		
		state = STATE_RESUME;
		
		init_glut_win();
		
		/* run glut main loop */
		glutMainLoop();
	}
	/* ctor */
	game_t() {
		
	}
};

/***************************************************************
 * MAIN FUNCTION
 ***************************************************************/

int main(int argc, const char * argv[])
{
	/* seed PRNG */
	srand(static_cast<unsigned int>(time(NULL)));
	
	/* init glut */
	glutInit(&argc, const_cast<char**>(argv));

	gGame = new game_t();
	
	/*
	 * do init after declaring the global instance because glut is
	 * retarded and doesn't let us pass a refcon.
	 */
	gGame->init();
	
    return 0;
}

