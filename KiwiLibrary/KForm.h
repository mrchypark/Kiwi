#pragma once

#define P_MIN -40.f

enum class KPOSTag : char
{
	UNKNOWN,
	NNG, NNP, NNB, 
	VV, VA, 
	MAG, 
	NR, NP,
	VX,
	MM, MAJ,
	IC,
	XPN, XSN, XSV, XSA, XR,
	VCP, VCN,
	SF, SP, SS, SE, SO, SW,
	NF, NV, NA,
	SL, SH, SN,
	JKS, JKC, JKG, JKO, JKB, JKV, JKQ, JX, JC,
	EP, EF, EC, ETN, ETM,
	V,
	MAX,
};

enum class KCondVowel : char
{
	none,
	any,
	vowel,
	vocalic,
	vocalicH,
	nonVowel,
	nonVocalic,
	nonVocalicH
};

enum class KCondPolarity : char
{
	none,
	positive,
	negative
};

KPOSTag makePOSTag(k_wstring tagStr);
const char* tagToString(KPOSTag t);
struct KForm;

struct KMorpheme
{
#ifdef _DEBUG
	static size_t uid;
	size_t id;
	string form;
#endif
	KMorpheme(const string& _form = "", 
		KPOSTag _tag = KPOSTag::UNKNOWN, 
		KCondVowel _vowel = KCondVowel::none,
		KCondPolarity _polar = KCondPolarity::none, 
		float _p = 0, char _combineSocket = 0)
		: tag(_tag), vowel(_vowel), polar(_polar), p(_p), combineSocket(_combineSocket)
#ifdef  _DEBUG
		, id(uid++), form(_form)
#endif //  _DEBUG
	{
	}

	KMorpheme(KMorpheme&& m)
	{
#ifdef _DEBUG
		id = m.id;
		swap(form, m.form);
#endif
		kform = m.kform;
		wform = m.wform;
		tag = m.tag;
		vowel = m.vowel;
		polar = m.polar;
		combineSocket = m.combineSocket;
		p = m.p;
		swap(chunks, m.chunks);
#ifdef USE_DIST_MAP
		swap(distMap, m.distMap);
#endif
	}

	~KMorpheme() 
	{ 
#ifdef USE_DIST_MAP
		if (distMap) delete distMap;
#endif
		if (chunks) delete chunks;
	}
	const string& getForm() const { return *kform; }
	const string* kform = nullptr;
	const k_wstring* wform = nullptr;
	KPOSTag tag = KPOSTag::UNKNOWN;
	KCondVowel vowel = KCondVowel::none;
	KCondPolarity polar = KCondPolarity::none;
	char combineSocket = 0;
	float p = 0;
	vector<const KMorpheme*>* chunks = nullptr;
	const KMorpheme* combined = nullptr;
#ifdef USE_DIST_MAP
	unordered_map<const KMorpheme*, float>* distMap = nullptr;
#endif
	void readFromBin(FILE* f, const function<const KMorpheme*(size_t)>& mapper);
	void writeToBin(FILE* f, const function<size_t(const KMorpheme*)>& mapper) const;
};

struct KForm
{
	string form;
	k_wstring wform;
	vector<const KMorpheme*> candidate;
	KCondVowel vowel = KCondVowel::none;
	KCondPolarity polar = KCondPolarity::none;
	bool hasFirstV = false;
	//float maxP = 0;
	unordered_set<char> suffix;
	KForm(const char* _form = nullptr);
	KForm(const string& _form) : form(_form) {}
	void updateCond();

	void readFromBin(FILE* f, const function<const KMorpheme*(size_t)>& mapper);
	void writeToBin(FILE* f, const function<size_t(const KMorpheme*)>& mapper) const;
};

