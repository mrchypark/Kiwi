﻿#include <fstream>
#include <random>

#include <kiwi/Kiwi.h>
#include <kiwi/Utils.h>
#include "ArchAvailable.h"
#include "KTrie.h"
#include "StrUtils.h"
#include "FileUtils.h"
#include "FrozenTrie.hpp"
#include "Knlm.hpp"
#include "serializer.hpp"
#include "count.hpp"
#include "FeatureTestor.h"
#include "Combiner.h"
#include "RaggedVector.hpp"
#include "SkipBigramTrainer.hpp"
#include "SkipBigramModel.hpp"
#include "SortUtils.hpp"

using namespace std;
using namespace kiwi;

KiwiBuilder::KiwiBuilder() = default;

KiwiBuilder::~KiwiBuilder() = default;

KiwiBuilder::KiwiBuilder(const KiwiBuilder&) = default;

KiwiBuilder::KiwiBuilder(KiwiBuilder&&) noexcept = default;

KiwiBuilder& KiwiBuilder::operator=(const KiwiBuilder&) = default;

KiwiBuilder& KiwiBuilder::operator=(KiwiBuilder&&) = default;

template<class Fn>
auto KiwiBuilder::loadMorphemesFromTxt(std::istream& is, Fn&& filter) -> MorphemeMap
{
	struct LongTail
	{
		KString form;
		float weight = 0;
		POSTag tag = POSTag::unknown;
		CondVowel cvowel = CondVowel::none;
		KString origForm;
		int addAlias = 0;
		size_t origMorphId = 0;

		LongTail(KString _form = {},
			float _weight = 0,
			POSTag _tag = POSTag::unknown,
			CondVowel _cvowel = CondVowel::none,
			KString _origForm = {},
			int _addAlias = 0,
			size_t _origMorphId = 0
		) :
			form{ _form }, weight{ _weight }, tag{ _tag }, cvowel{ _cvowel }, 
			origForm{ _origForm }, addAlias{ _addAlias }, origMorphId{ _origMorphId }
		{
		}
	};

	Vector<LongTail> longTails;
	UnorderedMap<POSTag, float> longTailWeights;
	MorphemeMap morphMap;

	const auto& insertMorph = [&](KString&& form, float score, POSTag tag, CondVowel cvowel, size_t origMorphemeId = 0)
	{
		auto& fm = addForm(form);
		bool unified = false;
		if (isEClass(tag) && form[0] == u'아')
		{
			form[0] = u'어';
			unified = true;
		}

		auto it = morphMap.find(make_pair(form, tag));
		if (it != morphMap.end())
		{
			fm.candidate.emplace_back(it->second);
			if (!unified) morphemes[it->second].kform = &fm - &forms[0];
		}
		else
		{
			size_t mid = morphemes.size();
			morphMap.emplace(make_pair(form, tag), origMorphemeId ? origMorphemeId : mid);
			fm.candidate.emplace_back(mid);
			morphemes.emplace_back(tag, cvowel, CondPolarity::none);
			morphemes.back().kform = &fm - &forms[0];
			morphemes.back().userScore = score;
			morphemes.back().lmMorphemeId = origMorphemeId;
		}
	};

	string line;
	while (getline(is, line))
	{
		auto wstr = utf8To16(line);
		if (!wstr.empty() && wstr.back() == '\n') wstr.pop_back();
		if (wstr.empty()) continue;
		auto fields = split(wstr, u'\t');
		if (fields.size() < 3)
		{
			throw Exception{ "wrong line: " + line };
		}

		auto form = normalizeHangul(fields[0]);
		auto tag = toPOSTag(fields[1]);
		if (clearIrregular(tag) >= POSTag::p || clearIrregular(tag) == POSTag::unknown)
		{
			cerr << "Wrong tag at line: " + line << endl;
			continue;
		}

		float morphWeight = stof(fields[2].begin(), fields[2].end());

		CondVowel cvowel = CondVowel::none;
		KString origMorphemeOfAlias;
		int addAlias = 0;
		if (fields.size() > 3)
		{
			for (size_t i = 3; i < fields.size(); ++i)
			{
				auto& f = fields[i];
				if (f == u"vowel")
				{
					if (cvowel != CondVowel::none) throw Exception{ "wrong line: " + line };
					cvowel = CondVowel::vowel;
					if (i + 1 < fields.size())
					{
						if (stof(fields[i + 1].begin(), fields[i + 1].end())) ++i;
					}
				}
				else if (f == u"non_vowel")
				{
					if (cvowel != CondVowel::none) throw Exception{ "wrong line: " + line };
					cvowel = CondVowel::non_vowel;
					if (i + 1 < fields.size())
					{
						if (stof(fields[i + 1].begin(), fields[i + 1].end())) ++i;
					}
				}
				else if (f == u"vocalic")
				{
					if (cvowel != CondVowel::none) throw Exception{ "wrong line: " + line };
					cvowel = CondVowel::vocalic;
					if (i + 1 < fields.size())
					{
						if (stof(fields[i + 1].begin(), fields[i + 1].end())) ++i;
					}
				}
				else if (f[0] == u'=')
				{
					if (!origMorphemeOfAlias.empty()) throw Exception{ "wrong line: " + line };
					if (f[1] == u'=')
					{
						origMorphemeOfAlias = normalizeHangul(f.substr(2));
						addAlias = 2; // ==의 경우 alias에 추가하고, score까지 동일하게 통일
					}
					else
					{
						origMorphemeOfAlias = normalizeHangul(f.substr(1));
						addAlias = 1; // =의 경우 alias에 추가하고, score는 discount
					}
					
				}
				else if (f[0] == u'~')
				{
					if (!origMorphemeOfAlias.empty()) throw Exception{ "wrong line: " + line };
					origMorphemeOfAlias = normalizeHangul(f.substr(1));
					addAlias = 0; // ~의 경우 말뭉치 로딩시에만 alias 처리하고, 실제 모델 데이터에는 삽입 안 함
				}
				else
				{
					throw Exception{ "wrong line: " + line };
				}
			}
		}

		if (filter(tag, morphWeight) && origMorphemeOfAlias.empty())
		{
			insertMorph(move(form), morphWeight, tag, cvowel);
		}
		else
		{
			longTails.emplace_back(LongTail{ form, morphWeight, tag, cvowel, origMorphemeOfAlias, addAlias });
			longTailWeights[tag] += morphWeight;
		}
		
	}

	for (LongTail& p : longTails)
	{
		if (p.origForm.empty()) continue;
		auto it = morphMap.find(make_pair(p.origForm, clearIrregular(p.tag)));
		auto it2 = morphMap.find(make_pair(p.origForm, setIrregular(p.tag)));
		
		if (it != morphMap.end() && it2 != morphMap.end())
		{
			throw Exception{ "ambiguous base morpheme: " + utf16To8(p.origForm) + "/" + tagToString(clearIrregular(p.tag)) };
		}
		it = (it == morphMap.end()) ? it2 : it;
		if (it == morphMap.end())
		{
			throw Exception{ "cannot find base morpheme: " + utf16To8(p.origForm) + "/" + tagToString(p.tag) };
		}
		p.origMorphId = it->second;
		if (!p.addAlias) continue;
		morphemes[it->second].userScore += p.weight;
	}

	for (LongTail& p : longTails)
	{
		if (p.origForm.empty())
		{
			insertMorph(move(p.form), log(p.weight / longTailWeights[p.tag]), p.tag, p.cvowel, getDefaultMorphemeId(p.tag));
		}
		else
		{
			if (p.addAlias)
			{
				float normalized = p.weight / morphemes[p.origMorphId].userScore;
				float score = log(normalized);
				if (p.addAlias > 1) score = 0;
				insertMorph(move(p.form), score, p.tag, p.cvowel, p.origMorphId);
			}
			else
			{
				morphMap.emplace(make_pair(move(p.form), p.tag), p.origMorphId);
			}
		}
	}
	for (auto& m : morphemes)
	{
		if (m.userScore <= 0) continue;
		m.userScore = 0;
	}

	for (auto& m : morphemes)
	{
		if (!isIrregular(m.tag)) continue;
		auto it = morphMap.find(make_pair(forms[m.kform].form, clearIrregular(m.tag)));
		if (it != morphMap.end()) continue;
		morphMap.emplace(
			make_pair(forms[m.kform].form, clearIrregular(m.tag)), 
			morphMap.find(make_pair(forms[m.kform].form, m.tag))->second
		);
	}
	return morphMap;
}

auto KiwiBuilder::restoreMorphemeMap() const -> MorphemeMap
{
	MorphemeMap ret;
	for (size_t i = defaultTagSize + 1; i < morphemes.size(); ++i)
	{
		size_t id = morphemes[i].lmMorphemeId;
		if (!id) id = i;
		ret.emplace(make_pair(forms[morphemes[i].kform].form, morphemes[i].tag), id);
	}
	for (auto& m : morphemes)
	{
		if (!isIrregular(m.tag)) continue;
		auto it = ret.find(make_pair(forms[m.kform].form, clearIrregular(m.tag)));
		if (it != ret.end()) continue;
		ret.emplace(
			make_pair(forms[m.kform].form, clearIrregular(m.tag)),
			ret.find(make_pair(forms[m.kform].form, m.tag))->second
		);
	}
	return ret;
}

void KiwiBuilder::addCorpusTo(RaggedVector<uint16_t>& out, std::istream& is, KiwiBuilder::MorphemeMap& morphMap)
{
	Vector<uint16_t> wids;
	string line;
	while (getline(is, line))
	{
		auto wstr = utf8To16(line);
		if (!wstr.empty() && wstr.back() == '\n') wstr.pop_back();
		if (wstr.empty() && wids.size() > 1)
		{
			out.emplace_back();
			out.add_data(0);
			out.insert_data(wids.begin(), wids.end());
			out.add_data(1);
			wids.clear();
			continue;
		}
		auto fields = split(wstr, u'\t');
		if (fields.size() < 2) continue;

		for (size_t i = 1; i < fields.size(); i += 2)
		{
			auto f = normalizeHangul(fields[i]);
			if (f.empty()) continue;

			auto t = toPOSTag(fields[i + 1]);

			if (f[0] == u'아' && fields[i + 1][0] == 'E')
			{
				f[0] = u'어';
			}

			auto it = morphMap.find(make_pair(f, t));
			if (it == morphMap.end() || !morphemes[it->second].chunks.empty() || morphemes[it->second].combineSocket)
			{
				if (t < POSTag::p && t != POSTag::unknown)
				{
					wids.emplace_back(getDefaultMorphemeId(t));
				}
				else
				{
					//wids.emplace_back(getDefaultMorphemeId(POSTag::nnp));
				}
			}
			else
			{
				wids.emplace_back(it->second);
			}
		}
	}
}

void KiwiBuilder::updateForms()
{
	vector<pair<FormRaw, size_t>> formOrder;
	vector<size_t> newIdcs(forms.size());

	for (size_t i = 0; i < forms.size(); ++i)
	{
		formOrder.emplace_back(move(forms[i]), i);
	}
	sort(formOrder.begin() + defaultTagSize - 1, formOrder.end());

	forms.clear();
	for (size_t i = 0; i < formOrder.size(); ++i)
	{
		forms.emplace_back(move(formOrder[i].first));
		newIdcs[formOrder[i].second] = i;
	}

	for (auto& m : morphemes)
	{
		m.kform = newIdcs[m.kform];
	}
}

void KiwiBuilder::updateMorphemes()
{
	for (auto& m : morphemes)
	{
		if (m.lmMorphemeId > 0) continue;
		if (m.tag == POSTag::p || (&m - morphemes.data() + m.combined) < langMdl.knlm->getHeader().vocab_size)
		{
			m.lmMorphemeId = &m - morphemes.data();
		}
		else
		{
			m.lmMorphemeId = getDefaultMorphemeId(m.tag);
		}
	}
}

void KiwiBuilder::loadMorphBin(std::istream& is)
{
	serializer::readMany(is, serializer::toKey("KIWI"), forms, morphemes);
	size_t cnt = 0;
	for (auto& form : forms)
	{
		formMap.emplace(form.form, cnt++);
	}
}

void KiwiBuilder::saveMorphBin(std::ostream& os) const
{
	serializer::writeMany(os, serializer::toKey("KIWI"), forms, morphemes);
}

KiwiBuilder::KiwiBuilder(const string& modelPath, size_t _numThreads, BuildOption _options, bool useSBG) 
	: detector{ modelPath, _numThreads }, options{ _options }, numThreads{ _numThreads ? _numThreads : thread::hardware_concurrency() }
{
	archType = getSelectedArch(ArchType::default_);

	{
		utils::MMap mm{ modelPath + "/sj.morph" };
		utils::imstream iss{ mm };
		loadMorphBin(iss);
	}
	langMdl.knlm = lm::KnLangModelBase::create(utils::MMap(modelPath + string{ "/sj.knlm" }), archType);
	if (useSBG)
	{
		langMdl.sbg = sb::SkipBigramModelBase::create(utils::MMap(modelPath + string{ "/skipbigram.mdl" }), archType);
	}

	if (!!(options & BuildOption::loadDefaultDict))
	{
		loadDictionary(modelPath + "/default.dict");
	}

	{
		ifstream ifs;
		combiningRule = make_shared<cmb::CompiledRule>(cmb::RuleSet{ openFile(ifs, modelPath + string{ "/combiningRule.txt" }) }.compile());
		addAllomorphsToRule();
	}
}

KiwiBuilder::KiwiBuilder(const ModelBuildArgs& args)
{
	archType = getSelectedArch(ArchType::default_);

	forms.resize(defaultTagSize - 1);
	morphemes.resize(defaultTagSize + 1); // additional places for <s> & </s>
	for (size_t i = 1; i < defaultTagSize; ++i)
	{
		forms[i - 1].candidate.emplace_back(i + 1);
		morphemes[i + 1].tag = (POSTag)i;
	}

	ifstream ifs;
	auto realMorph = loadMorphemesFromTxt(openFile(ifs, args.morphemeDef), [&](POSTag tag, float cnt)
	{
		return cnt >= args.minMorphCnt;
	});
	updateForms();

	RaggedVector<utils::Vid> sents;
	for (auto& path : args.corpora)
	{
		ifstream ifs;
		addCorpusTo(sents, openFile(ifs, path), realMorph);
	}

	if (args.dropoutProb > 0 && args.dropoutSampling > 0)
	{
		mt19937_64 rng{ 42 };
		bernoulli_distribution sampler{ args.dropoutSampling }, drop{ args.dropoutProb };

		size_t origSize = sents.size();
		for (size_t i = 0; i < origSize; ++i)
		{
			if (!sampler(rng)) continue;
			sents.emplace_back();
			for (size_t j = 0; j < sents[i].size(); ++j)
			{
				auto v = sents[i][j];
				if (drop(rng))
				{
					v = getDefaultMorphemeId(clearIrregular(morphemes[v].tag));
				}
				sents.add_data(v);
			}
		}
	}
	
	size_t lmVocabSize = 0;
	for (auto& p : realMorph) lmVocabSize = max(p.second, lmVocabSize);
	lmVocabSize += 1;

	Vector<utils::Vid> historyTx(lmVocabSize);
	if (args.useLmTagHistory)
	{
		for (size_t i = 0; i < lmVocabSize; ++i)
		{
			historyTx[i] = (size_t)clearIrregular(morphemes[i].tag) + lmVocabSize;
		}
	}

	vector<pair<uint16_t, uint16_t>> bigramList;
	utils::ThreadPool pool;
	if (args.numWorkers > 1)
	{
		pool.~ThreadPool();
		new (&pool) utils::ThreadPool{ args.numWorkers };
	}
	auto cntNodes = utils::count(sents.begin(), sents.end(), args.lmMinCnt, 1, args.lmOrder, (args.numWorkers > 1 ? &pool : nullptr), &bigramList, args.useLmTagHistory ? &historyTx : nullptr);
	cntNodes.root().getNext(lmVocabSize)->val /= 2;
	langMdl.knlm = lm::KnLangModelBase::create(lm::KnLangModelBase::build(
		cntNodes, 
		args.lmOrder, args.lmMinCnt, args.lmLastOrderMinCnt, 
		2, 0, 1, 1e-5, 
		args.quantizeLm ? 8 : 0,
		args.compressLm,
		&bigramList, 
		args.useLmTagHistory ? &historyTx : nullptr
	), archType);

	updateMorphemes();
}

class SBDataFeeder
{
	const RaggedVector<utils::Vid>& sents;
	const lm::KnLangModelBase* lm = nullptr;
	Vector<Vector<float>> lmBuf;

public:
	SBDataFeeder(const RaggedVector<utils::Vid>& _sents, const lm::KnLangModelBase* _lm, size_t numThreads = 1)
		: sents{ _sents }, lm{ _lm }, lmBuf(numThreads)
	{
	}

	sb::FeedingData<utils::Vid> operator()(size_t i, size_t threadId = 0)
	{
		sb::FeedingData<utils::Vid> ret;
		ret.len = sents[i].size();
		if (lmBuf[threadId].size() < ret.len)
		{
			lmBuf[threadId].resize(ret.len);
		}
		ret.x = &sents[i][0];
		lm->evaluate(sents[i].begin(), sents[i].end(), lmBuf[threadId].data());
		ret.lmLogProbs = lmBuf[threadId].data();
		return ret;
	}
};

KiwiBuilder::KiwiBuilder(const string& modelPath, const ModelBuildArgs& args)
	: KiwiBuilder{ modelPath }
{
	auto realMorph = restoreMorphemeMap();
	sb::SkipBigramTrainer<utils::Vid, 8> sbg;
	RaggedVector<utils::Vid> sents;
	for (auto& path : args.corpora)
	{
		ifstream ifs;
		addCorpusTo(sents, openFile(ifs, path), realMorph);
	}

	if (args.dropoutProb > 0 && args.dropoutSampling > 0)
	{
		mt19937_64 rng{ 42 };
		bernoulli_distribution sampler{ args.dropoutSampling };
		discrete_distribution<> drop{ { 1 - args.dropoutProb, args.dropoutProb / 3, args.dropoutProb / 3, args.dropoutProb / 3} };

		size_t origSize = sents.size();
		for (size_t n = 0; n < 2; ++n)
		{
			for (size_t i = 0; i < origSize; ++i)
			{
				//if (!sampler(rng)) continue;
				sents.emplace_back();
				sents.add_data(sents[i][0]);
				bool emptyDoc = true;
				for (size_t j = 1; j < sents[i].size() - 1; ++j)
				{
					auto v = sents[i][j];
					switch (drop(rng))
					{
					case 0: // no dropout
						emptyDoc = false;
						sents.add_data(v);
						break;
					case 1: // replacement
						emptyDoc = false;
						sents.add_data(getDefaultMorphemeId(morphemes[v].tag));
						break;
					case 2: // deletion
						break;
					case 3: // insertion
						emptyDoc = false;
						sents.add_data(getDefaultMorphemeId(morphemes[v].tag));
						sents.add_data(v);
						break;
					}
				}

				if (emptyDoc)
				{
					sents.pop_back();
				}
				else
				{
					sents.add_data(sents[i][sents[i].size() - 1]);
				}
			}
		}
	}

	size_t lmVocabSize = 0;
	for (auto& p : realMorph) lmVocabSize = max(p.second, lmVocabSize);
	lmVocabSize += 1;

	auto sbgTokenFilter = [&](size_t a)
	{
		auto tag = morphemes[a].tag;
		if (isEClass(tag) || isJClass(tag)) return false;
		if (tag == POSTag::vcp || tag == POSTag::vcn) return false;
		if (isVerbClass(tag) && forms[morphemes[a].kform].form == u"하") return false;
		return true;
	};

	auto sbgPairFilter = [&](size_t a, size_t b)
	{
		if (a < 21 || (34 < a && a < defaultTagSize + 1)) return false;
		if ((1 < b && b < 21) || (34 < b && b < defaultTagSize + 1)) return false;
		return true;
	};

	sbg = sb::SkipBigramTrainer<utils::Vid, 8>{ sents, sbgTokenFilter, sbgPairFilter, 0, 150, 20, true, 0.333f, 1, 1000000 };
	Vector<float> lmLogProbs;
	auto tc = sbg.newContext();
	float llMean = 0;
	size_t llCnt = 0;
	Vector<size_t> sampleIdcs;
	for (size_t i = 0; i < sents.size(); ++i)
	{
		if (i % 20 == 0) continue;
		sampleIdcs.emplace_back(i);
	}

	for (size_t i = 0; i < sents.size(); i += 20)
	{
		auto sent = sents[i];
		if (lmLogProbs.size() < sent.size())
		{
			lmLogProbs.resize(sent.size());
		}
		langMdl.knlm->evaluate(sent.begin(), sent.end(), lmLogProbs.begin());
		//float sum = sbg.evaluate(&sent[0], lmLogProbs.data(), sent.size());
		float sum = accumulate(lmLogProbs.begin() + 1, lmLogProbs.begin() + sent.size(), 0.);
		size_t cnt = sent.size() - 1;
		llCnt += cnt;
		llMean += (sum - llMean * cnt) / llCnt;
	}
	cout << "Init Dev AvgLL: " << llMean << endl;

	llCnt = 0;
	llMean = 0;
	float lrStart = 1e-1;
	size_t numEpochs = 10;
	size_t totalSteps = sampleIdcs.size() * numEpochs;

	if (args.numWorkers <= 1)
	{
		sbg.train(SBDataFeeder{ sents, langMdl.knlm.get() }, [&](const sb::ObservingData& od)
			{
				llCnt += od.cntRecent;
				llMean += (od.llRecent - llMean * od.cntRecent) / llCnt;
				if (od.globalStep % 10000 == 0)
				{
					cout << od.globalStep / 10000 << " (" << std::round(od.globalStep * 1000. / totalSteps) / 10 << "%): AvgLL: " << od.llMeanTotal << ", RecentLL: " << llMean << endl;
					llCnt = 0;
					llMean = 0;
				}
			}, sampleIdcs, totalSteps, lrStart);
	}
	else
	{
		sbg.trainMulti(args.numWorkers, SBDataFeeder{ sents, langMdl.knlm.get(), 8 }, [&](const sb::ObservingData& od)
			{
				llCnt += od.cntRecent;
				llMean += (od.llRecent - llMean * od.cntRecent) / llCnt;
				if (od.prevGlobalStep / 10000 < od.globalStep / 10000)
				{
					cout << od.globalStep / 10000 << " (" << std::round(od.globalStep * 1000. / totalSteps) / 10 << "%): AvgLL: " << od.llMeanTotal << ", RecentLL: " << llMean << endl;
					llCnt = 0;
					llMean = 0;
				}
			}, sampleIdcs, totalSteps, lrStart);
	}

	{
		ofstream ofs{ "sbg.fin.bin", ios_base::binary };
		sbg.save(ofs);
	}

	llCnt = 0;
	llMean = 0;
	for (size_t i = 0; i < sents.size(); i += 20)
	{
		auto sent = sents[i];
		if (lmLogProbs.size() < sent.size())
		{
			lmLogProbs.resize(sent.size());
		}
		langMdl.knlm->evaluate(sent.begin(), sent.end(), lmLogProbs.begin());
		float sum = sbg.evaluate(&sent[0], lmLogProbs.data(), sent.size());
		size_t cnt = sent.size() - 1;
		llCnt += cnt;
		llMean += (sum - llMean * cnt) / llCnt;
	}
	cout << "After Dev AvgLL: " << llMean << endl;

	ofstream ofs{ modelPath + "/sbg.result.log" };
	sbg.printParameters(ofs << "AvgLL: " << llMean << "\n", [&](size_t v)
	{
		return utf16To8(joinHangul(forms[morphemes[v].kform].form)) + "/" + tagToString(morphemes[v].tag);
	});
	
	{
		auto mem = sbg.convertToModel();
		ofstream ofs{ modelPath + "/skipbigram.mdl", ios_base::binary };
		ofs.write((const char*)mem.get(), mem.size());
	}
}

void KiwiBuilder::saveModel(const string& modelPath) const
{
	{
		ofstream ofs{ modelPath + "/sj.morph", ios_base::binary };
		saveMorphBin(ofs);
	}
	{
		auto mem = langMdl.knlm->getMemory();
		ofstream ofs{ modelPath + "/sj.knlm", ios_base::binary };
		ofs.write((const char*)mem.get(), mem.size());
	}
}

void KiwiBuilder::addAllomorphsToRule()
{
	UnorderedMap<size_t, Vector<const MorphemeRaw*>> allomorphs;
	for (auto& m : morphemes)
	{
		if (!isJClass(m.tag) && !isEClass(m.tag)) continue;
		if (m.vowel() == CondVowel::none) continue;
		if (m.lmMorphemeId == getDefaultMorphemeId(m.tag)) continue;
		allomorphs[m.lmMorphemeId].emplace_back(&m);
	}

	for (auto& p : allomorphs)
	{
		if (p.second.size() <= 1) continue;
		vector<pair<U16StringView, CondVowel>> d;
		for (auto m : p.second)
		{
			d.emplace_back(forms[m->kform].form, m->vowel());
		}
		combiningRule->addAllomorph(d, p.second[0]->tag);
	}
}

FormRaw& KiwiBuilder::addForm(const KString& form)
{
	auto ret = formMap.emplace(form, forms.size());
	if (ret.second)
	{
		forms.emplace_back(form);
	}
	return forms[ret.first->second];
}

size_t KiwiBuilder::addForm(Vector<FormRaw>& newForms, UnorderedMap<KString, size_t>& newFormMap, KString form) const
{
	auto it = formMap.find(form);
	if (it != formMap.end())
	{
		return it->second;
	}
	auto ret = newFormMap.emplace(form, forms.size() + newForms.size());
	if (ret.second)
	{
		newForms.emplace_back(form);
	}
	return ret.first->second;
}

bool KiwiBuilder::addWord(U16StringView newForm, POSTag tag, float score, size_t origMorphemeId)
{
	if (newForm.empty()) return false;

	auto normalizedForm = normalizeHangul(newForm);
	auto& f = addForm(normalizedForm);
	if (f.candidate.empty())
	{
	}
	else
	{
		for (auto p : f.candidate)
		{
			// if `form` already has the same `tag`, skip adding
			if (morphemes[p].tag == tag && morphemes[p].lmMorphemeId == origMorphemeId) return false;
		}
	}

	f.candidate.emplace_back(morphemes.size());
	morphemes.emplace_back(tag);
	auto& newMorph = morphemes.back();
	newMorph.kform = &f - &forms[0];
	newMorph.userScore = score;
	newMorph.lmMorphemeId = origMorphemeId;
	return true;
}

bool KiwiBuilder::addWord(const std::u16string& newForm, POSTag tag, float score, size_t origMorphemeId)
{
	return addWord(nonstd::to_string_view(newForm), tag, score, origMorphemeId);
}

void KiwiBuilder::addCombinedMorphemes(
	Vector<FormRaw>& newForms, 
	UnorderedMap<KString, size_t>& newFormMap, 
	Vector<MorphemeRaw>& newMorphemes, 
	UnorderedMap<size_t, Vector<uint32_t>>& newFormCands,
	size_t leftId, 
	size_t rightId, 
	size_t ruleId
) const
{
	const auto& getMorph = [&](size_t id) -> const MorphemeRaw&
	{
		if (id < morphemes.size()) return morphemes[id];
		else return newMorphemes[id - morphemes.size()];
	};

	const auto& getForm = [&](size_t id) -> const FormRaw&
	{
		if (id < forms.size()) return forms[id];
		else return newForms[id - forms.size()];
	};

	auto res = combiningRule->combine(getForm(getMorph(leftId).kform).form, getForm(getMorph(rightId).kform).form, ruleId);
	for (auto& r : res)
	{
		size_t newId = morphemes.size() + newMorphemes.size();
		newMorphemes.emplace_back(POSTag::unknown);
		auto& newMorph = newMorphemes.back();
		newMorph.lmMorphemeId = newId;
		if (getMorph(leftId).chunks.empty())
		{
			newMorph.chunks.emplace_back(leftId);
			newMorph.chunkPositions.emplace_back(0, r.leftEnd);
			newMorph.userScore = getMorph(leftId).userScore + r.score;
		}
		else
		{
			auto& leftMorph = getMorph(leftId);
			newMorph.chunks = leftMorph.chunks;
			newMorph.chunkPositions = leftMorph.chunkPositions;
			newMorph.chunkPositions.back().second = r.leftEnd;
			if (r.vowel == CondVowel::none) r.vowel = leftMorph.vowel();
			if (r.polar == CondPolarity::none) r.polar = leftMorph.polar();
			newMorph.userScore = leftMorph.userScore + r.score;
		}
		newMorph.chunks.emplace_back(rightId);
		newMorph.chunkPositions.emplace_back(r.rightBegin, r.str.size());
		if (getMorph(newMorph.chunks[0]).combineSocket)
		{
			newMorph.combineSocket = getMorph(newMorph.chunks[0]).combineSocket;
		}
		newMorph.userScore += getMorph(rightId).userScore;
		newMorph.setVowel(r.vowel);
		// 양/음성 조건은 부분결합된 형태소에서만 유효
		if (getMorph(leftId).tag == POSTag::p)
		{
			newMorph.setPolar(r.polar);
		}
		size_t fid = addForm(newForms, newFormMap, r.str);
		newFormCands[fid].emplace_back(newId);
		newMorph.kform = fid;
	}
}

void KiwiBuilder::buildCombinedMorphemes(
	Vector<FormRaw>& newForms, 
	Vector<MorphemeRaw>& newMorphemes, 
	UnorderedMap<size_t, Vector<uint32_t>>& newFormCands
) const
{
	const auto& getMorph = [&](size_t id) -> const MorphemeRaw&
	{
		if (id < morphemes.size()) return morphemes[id];
		else return newMorphemes[id - morphemes.size()];
	};

	const auto& getForm = [&](size_t id) -> const FormRaw&
	{
		if (id < forms.size()) return forms[id];
		else return newForms[id - forms.size()];
	};

	UnorderedMap<KString, size_t> newFormMap;
	Vector<Vector<size_t>> combiningLeftCands, combiningRightCands;
	UnorderedMap<std::tuple<KString, POSTag, CondPolarity>, size_t> combiningSuffices;
	size_t combiningUpdateIdx = defaultTagSize + 2;

	auto ruleLeftIds = combiningRule->getRuleIdsByLeftTag();
	auto ruleRightIds = combiningRule->getRuleIdsByRightTag();
	while (combiningUpdateIdx < morphemes.size() + newMorphemes.size())
	{
		// 새 형태소들 중에서 결합이 가능한 형태소 후보 추출
		for (size_t i = combiningUpdateIdx; i < morphemes.size() + newMorphemes.size(); ++i)
		{
			auto& morph = getMorph(i);
			auto tag = morph.tag;
			auto& form = getForm(morph.kform).form;

			if (clearIrregular(tag) > POSTag::pa) continue;
			if (morph.combined) continue;

			if (tag == POSTag::unknown)
			{
				if (morph.chunks.empty()) continue;
				tag = getMorph(morph.chunks.back()).tag;
			}

			for (size_t feat = 0; feat < 4; ++feat)
			{
				for (auto id : ruleLeftIds[make_tuple(tag, feat)])
				{
					auto res = combiningRule->testLeftPattern(form, id);
					if (res.empty()) continue;

					if (combiningLeftCands.size() <= id) combiningLeftCands.resize(id + 1);
					if (combiningLeftCands[id].empty() || combiningLeftCands[id].back() < i) combiningLeftCands[id].emplace_back(i);
				}
			}

			if (morph.tag == POSTag::unknown) continue;
			for (auto id : ruleRightIds[tag])
			{
				auto res = combiningRule->testRightPattern(form, id);
				if (res.empty()) continue;

				if (combiningRightCands.size() <= id) combiningRightCands.resize(id + 1);
				combiningRightCands[id].emplace_back(i);
			}

			if (tag == POSTag::vv || tag == POSTag::va || tag == POSTag::vvi || tag == POSTag::vai)
			{
				CondVowel vowel = CondVowel::none;
				CondPolarity polar = FeatureTestor::isMatched(&form, CondPolarity::positive) ? CondPolarity::positive : CondPolarity::negative;

				POSTag partialTag;
				switch (tag)
				{
				case POSTag::vv:
					partialTag = POSTag::pv;
					break;
				case POSTag::va:
					partialTag = POSTag::pa;
					break;
				case POSTag::vvi:
					partialTag = POSTag::pvi;
					break;
				case POSTag::vai:
					partialTag = POSTag::pai;
					break;
				default:
					break;
				}

				auto& ids = ruleLeftIds[make_tuple(partialTag, cmb::CompiledRule::toFeature(vowel, polar))];
				Vector<uint8_t> partialFormInserted(form.size());
				auto cform = form;

				for (auto id : ids)
				{
					auto res = combiningRule->testLeftPattern(form, id);
					if (res.empty()) continue;

					auto& startPos = get<1>(res[0]);
					auto& condPolar = get<2>(res[0]);

					if (startPos == 0)
					{
						if (combiningLeftCands.size() <= id) combiningLeftCands.resize(id + 1);
						combiningLeftCands[id].emplace_back(i);
					}
					else
					{
						auto inserted = combiningSuffices.emplace(
							make_tuple(cform.substr(startPos), tag, condPolar),
							0
						);
						if (inserted.second)
						{
							size_t fid = addForm(newForms, newFormMap, get<0>(inserted.first->first));
							size_t newId = morphemes.size() + newMorphemes.size();
							inserted.first->second = newId;
							newMorphemes.emplace_back(partialTag);
							auto& newMorph = newMorphemes.back();
							newMorph.kform = fid;
							newMorph.lmMorphemeId = newId;
							newMorph.combineSocket = combiningSuffices.size();
						}

						if (!partialFormInserted[startPos])
						{
							size_t fid = addForm(newForms, newFormMap, cform.substr(0, startPos));
							ptrdiff_t newId = (ptrdiff_t)(morphemes.size() + newMorphemes.size());
							newMorphemes.emplace_back(POSTag::p);
							auto& newMorph = newMorphemes.back();
							newMorph.kform = fid;
							newMorph.lmMorphemeId = newId;
							newMorph.combineSocket = getMorph(inserted.first->second).combineSocket;
							newMorph.combined = (ptrdiff_t)i - newId;
							newFormCands[fid].emplace_back(newId);
							partialFormInserted[startPos] = 1;
						}
					}
				}
			}
		}
		for (auto& p : combiningSuffices)
		{
			newMorphemes[p.second - morphemes.size()].tag = POSTag::p;
		}

		size_t updated = morphemes.size() + newMorphemes.size();

		// 규칙에 의한 결합 연산 수행 후 형태소 목록에 삽입
		for (size_t ruleId = 0; ruleId < min(combiningLeftCands.size(), combiningRightCands.size()); ++ruleId)
		{
			auto& ls = combiningLeftCands[ruleId];
			auto lmid = lower_bound(ls.begin(), ls.end(), combiningUpdateIdx);
			auto& rs = combiningRightCands[ruleId];
			auto rmid = lower_bound(rs.begin(), rs.end(), combiningUpdateIdx);
			for (auto lit = lmid; lit != ls.end(); ++lit)
			{
				for (auto rit = rs.begin(); rit != rmid; ++rit)
				{
					addCombinedMorphemes(newForms, newFormMap, newMorphemes, newFormCands, *lit, *rit, ruleId);
				}
			}

			for (auto lit = ls.begin(); lit != ls.end(); ++lit)
			{
				for (auto rit = rmid; rit != rs.end(); ++rit)
				{
					addCombinedMorphemes(newForms, newFormMap, newMorphemes, newFormCands, *lit, *rit, ruleId);
				}
			}
		}
		combiningUpdateIdx = updated;
	}
}

bool KiwiBuilder::addWord(U16StringView form, POSTag tag, float score)
{
	return addWord(form, tag, score, getDefaultMorphemeId(tag));
}

bool KiwiBuilder::addWord(const u16string& form, POSTag tag, float score)
{
	return addWord(nonstd::to_string_view(form), tag, score);
}

bool KiwiBuilder::addWord(const char16_t* form, POSTag tag, float score)
{
	return addWord(U16StringView{ form }, tag, score);
}

size_t KiwiBuilder::findMorpheme(U16StringView form, POSTag tag) const
{
	auto normalized = normalizeHangul(form);
	auto it = formMap.find(normalized);
	if (it == formMap.end()) return -1;

	for (auto p : forms[it->second].candidate)
	{
		if (morphemes[(size_t)p].tag == tag)
		{
			return p;
		}
	}
	return -1;
}

bool KiwiBuilder::addWord(U16StringView newForm, POSTag tag, float score, U16StringView origForm)
{
	size_t origMorphemeId = findMorpheme(origForm, tag);

	if (origMorphemeId == -1)
	{
		throw UnknownMorphemeException{ "cannot find the original morpheme " + utf16To8(origForm) + "/" + tagToString(tag) };
	}

	return addWord(newForm, tag, score, origMorphemeId);
}

bool KiwiBuilder::addWord(const u16string& newForm, POSTag tag, float score, const u16string& origForm)
{
	return addWord(nonstd::to_string_view(newForm), tag, score, origForm);
}

bool KiwiBuilder::addWord(const char16_t* newForm, POSTag tag, float score, const char16_t* origForm)
{
	return addWord(U16StringView(newForm), tag, score, U16StringView(origForm));
}

template<class U16>
bool KiwiBuilder::addPreAnalyzedWord(U16StringView form, const vector<pair<U16, POSTag>>& analyzed, vector<pair<size_t, size_t>> positions, float score)
{
	if (form.empty()) return false;

	Vector<uint32_t> analyzedIds;
	for (auto& p : analyzed)
	{
		size_t morphemeId = findMorpheme(p.first, p.second);
		if (morphemeId == -1)
		{
			throw UnknownMorphemeException{ "cannot find the original morpheme " + utf16To8(p.first) + "/" + tagToString(p.second) };
		}
		analyzedIds.emplace_back(morphemeId);
	}

	while (positions.size() < analyzed.size())
	{
		positions.emplace_back(0, form.size());
	}

	auto normalized = normalizeHangulWithPosition(form);

	for (auto& p : positions)
	{
		p.first = normalized.second[p.first];
		p.second = normalized.second[p.second];
	}

	auto& f = addForm(normalized.first);
	if (f.candidate.empty())
	{
	}
	else
	{
		for (auto p : f.candidate)
		{
			auto& mchunks = morphemes[p].chunks;
			if (mchunks == analyzedIds) return false;
		}
	}

	f.candidate.emplace_back(morphemes.size());
	morphemes.emplace_back(POSTag::unknown);
	auto& newMorph = morphemes.back();
	newMorph.kform = &f - &forms[0];
	newMorph.userScore = score;
	newMorph.lmMorphemeId = morphemes.size() - 1;
	newMorph.chunks = analyzedIds;
	newMorph.chunkPositions.insert(newMorph.chunkPositions.end(), positions.begin(), positions.end());
	return true;
}

bool KiwiBuilder::addPreAnalyzedWord(const u16string& form, const vector<pair<u16string, POSTag>>& analyzed, vector<pair<size_t, size_t>> positions, float score)
{
	return addPreAnalyzedWord(nonstd::to_string_view(form), analyzed, positions, score);
}

bool KiwiBuilder::addPreAnalyzedWord(const char16_t* form, const vector<pair<const char16_t*, POSTag>>& analyzed, vector<pair<size_t, size_t>> positions, float score)
{
	return addPreAnalyzedWord(U16StringView{ form }, analyzed, positions, score);
}

size_t KiwiBuilder::loadDictionary(const string& dictPath)
{
	size_t addedCnt = 0;
	ifstream ifs;
	openFile(ifs, dictPath);
	string line;
	array<U16StringView, 3> fields;
	u16string wstr;
	for (size_t lineNo = 1; getline(ifs, line); ++lineNo)
	{
		utf8To16(nonstd::to_string_view(line), wstr);
		while (!wstr.empty() && kiwi::identifySpecialChr(wstr.back()) == POSTag::unknown) wstr.pop_back();
		if (wstr.empty()) continue;
		if (wstr[0] == u'#') continue;
		size_t fieldSize = split(wstr, u'\t', fields.begin(), 2) - fields.begin();
		if (fieldSize < 2)
		{
			throw Exception("[loadUserDictionary] Wrong dictionary format at line " + to_string(lineNo) + " : " + line);
		}

		if (fields[0].find(' ') != fields[0].npos)
		{
			throw Exception("[loadUserDictionary] Form should not contain space. at line " + to_string(lineNo) + " : " + line);
		}

		float score = 0.f;
		if (fieldSize > 2) score = stof(fields[2].begin(), fields[2].end());

		if (fields[1].find(u'/') != fields[1].npos)
		{
			vector<pair<U16StringView, POSTag>> morphemes;

			for (auto& m : split(fields[1], u'+'))
			{
				size_t b = 0, e = m.size();
				while (b < e && m[e - 1] == ' ') --e;
				while (b < e && m[b] == ' ') ++b;
				m = m.substr(b, e - b);

				size_t p = m.rfind(u'/');
				if (p == m.npos)
				{
					throw Exception("[loadUserDictionary] Wrong dictionary format at line " + to_string(lineNo) + " : " + line);
				}
				auto pos = toPOSTag(m.substr(p + 1));
				if (pos == POSTag::max)
				{
					throw Exception("[loadUserDictionary] Unknown Tag '" + utf16To8(fields[1]) + "' at line " + to_string(lineNo));
				}
				morphemes.emplace_back(m.substr(0, p), pos);
			}

			if (morphemes.size() > 1)
			{
				addedCnt += addPreAnalyzedWord(fields[0], morphemes, {}, score) ? 1 : 0;
			}
			else
			{
				addedCnt += addWord(fields[0], morphemes[0].second, score, morphemes[0].first);
			}
		}
		else
		{
			auto pos = toPOSTag(fields[1]);
			if (pos == POSTag::max)
			{
				throw Exception("[loadUserDictionary] Unknown Tag '" + utf16To8(fields[1]) + "' at line " + to_string(lineNo));
			}
			addedCnt += addWord(fields[0], pos, score) ? 1 : 0;
		}
	}
	return addedCnt;
}

template<ArchType archType>
utils::FrozenTrie<kchar_t, const Form*> freezeTrie(utils::ContinuousTrie<KTrie>&& trie)
{
	return { trie, ArchTypeHolder<archType>{} };
}

using FnFreezeTrie = decltype(&freezeTrie<ArchType::none>);

struct FreezeTrieGetter
{
	template<std::ptrdiff_t i>
	struct Wrapper
	{
		static constexpr FnFreezeTrie value = &freezeTrie<static_cast<ArchType>(i)>;
	};
};

inline CondVowel reduceVowel(CondVowel v, const Morpheme* m)
{
	if (v == m->vowel) return v;
	if (CondVowel::vowel <= v && v <= CondVowel::vocalic_h)
	{
		if (CondVowel::vowel <= m->vowel && m->vowel <= CondVowel::vocalic_h)
		{
			return max(v, m->vowel);
		}
		return CondVowel::none;
	}
	else if (CondVowel::non_vowel <= v && v <= CondVowel::non_vocalic_h)
	{
		if (CondVowel::non_vowel <= m->vowel && m->vowel <= CondVowel::non_vocalic_h)
		{
			return min(v, m->vowel);
		}
		return CondVowel::none;
	}
	return CondVowel::none;
}

inline CondPolarity reducePolar(CondPolarity p, const Morpheme* m)
{
	if (p == m->polar) return p;
	return CondPolarity::none;
}

Kiwi KiwiBuilder::build(const TypoTransformer& typos, float typoCostThreshold) const
{
	Kiwi ret{ archType, langMdl, !typos.empty()};

	Vector<FormRaw> combinedForms;
	Vector<MorphemeRaw> combinedMorphemes;
	UnorderedMap<size_t, Vector<uint32_t>> newFormCands;

	buildCombinedMorphemes(combinedForms, combinedMorphemes, newFormCands);

	ret.forms.reserve(forms.size() + combinedForms.size() + 1);
	ret.morphemes.reserve(morphemes.size() + combinedMorphemes.size());
	ret.combiningRule = combiningRule;
	ret.integrateAllomorph = !!(options & BuildOption::integrateAllomorph);
	if (numThreads >= 1)
	{
		ret.pool = make_unique<utils::ThreadPool>(numThreads);
	}

	for (auto& f : forms)
	{
		auto it = newFormCands.find(ret.forms.size());
		if (it == newFormCands.end())
		{
			ret.forms.emplace_back(bake(f, ret.morphemes.data()));
		}
		else
		{
			ret.forms.emplace_back(bake(f, ret.morphemes.data(), it->second));
		}
		
	}
	for (auto& f : combinedForms)
	{
		ret.forms.emplace_back(bake(f, ret.morphemes.data(), newFormCands[ret.forms.size()]));
	}

	Vector<size_t> newFormIdMapper(ret.forms.size());
	iota(newFormIdMapper.begin(), newFormIdMapper.begin() + defaultTagSize - 1, 0);
	utils::sortWriteInvIdx(ret.forms.begin() + defaultTagSize - 1, ret.forms.end(), newFormIdMapper.begin() + defaultTagSize - 1, (size_t)(defaultTagSize - 1));
	ret.forms.emplace_back();

	uint8_t formHash = 0;
	for (size_t i = 1; i < ret.forms.size(); ++i)
	{
		if (ret.forms[i].form != ret.forms[i - 1].form) ++formHash;
		ret.forms[i].formHash = formHash;
	}

	for (auto& m : morphemes)
	{
		ret.morphemes.emplace_back(bake(m, ret.morphemes.data(), ret.forms.data(), newFormIdMapper));
	}
	for (auto& m : combinedMorphemes)
	{
		ret.morphemes.emplace_back(bake(m, ret.morphemes.data(), ret.forms.data(), newFormIdMapper));
	}

	utils::ContinuousTrie<KTrie> formTrie{ defaultTagSize + 1 };
	// reserve places for root node + default tag morphemes
	for (size_t i = 1; i <= defaultTagSize; ++i)
	{
		formTrie[i].val = &ret.forms[i - 1];
	}

	Vector<const Form*> sortedForms;
	for (size_t i = defaultTagSize; i < ret.forms.size() - 1; ++i)
	{
		auto& f = ret.forms[i];
		if (f.candidate.empty()) continue;

		if (f.candidate[0]->vowel != CondVowel::none)
		{
			f.vowel = accumulate(f.candidate.begin(), f.candidate.end(), f.candidate[0]->vowel, reduceVowel);
		}

		if (f.candidate[0]->polar != CondPolarity::none)
		{
			f.polar = accumulate(f.candidate.begin(), f.candidate.end(), f.candidate[0]->polar, reducePolar);
		}
		sortedForms.emplace_back(&f);
	}

	// 오타 교정이 없는 경우 일반 Trie 생성
	if (typos.empty())
	{
		sort(sortedForms.begin(), sortedForms.end(), [](const Form* a, const Form* b)
		{
			return a->form < b->form;
		});

		size_t estimatedNodeSize = 0;
		const KString* prevForm = nullptr;
		for (auto f : sortedForms)
		{
			if (!prevForm)
			{
				estimatedNodeSize += f->form.size();
				prevForm = &f->form;
				continue;
			}
			size_t commonPrefix = 0;
			while (commonPrefix < std::min(prevForm->size(), f->form.size())
				&& (*prevForm)[commonPrefix] == f->form[commonPrefix]) ++commonPrefix;
			estimatedNodeSize += f->form.size() - commonPrefix;
			prevForm = &f->form;
		}
		formTrie.reserveMore(estimatedNodeSize);

		decltype(formTrie)::CacheStore<const KString> cache;
		for (auto f : sortedForms)
		{
			formTrie.buildWithCaching(f->form, f, cache);
		}
	}
	// 오타 교정이 있는 경우 가능한 모든 오타에 대해 Trie 생성
	else
	{
		using TypoInfo = tuple<uint32_t, float, CondVowel>;
		UnorderedMap<KString, Vector<TypoInfo>> typoGroup;
		auto ptypos = typos.prepare();
		for (auto f : sortedForms)
		{
			for (auto t : ptypos._generate(f->form, typoCostThreshold))
			{
				if (t.leftCond != CondVowel::none && f->vowel != CondVowel::none && t.leftCond != f->vowel) continue;
				typoGroup[t.str].emplace_back(f - ret.forms.data(), t.cost, t.leftCond);
			}
		}

		Vector<decltype(typoGroup)::pointer> typoGroupSorted;
		size_t totTfSize = 0;
		for (auto& v : typoGroup)
		{
			typoGroupSorted.emplace_back(&v);
			sort(v.second.begin(), v.second.end(), [](const TypoInfo& a, const TypoInfo& b)
				{
					if (get<1>(a) < get<1>(b)) return true;
					if (get<1>(a) > get<1>(b)) return false;
					return get<0>(a) < get<0>(b);
				});
			totTfSize += v.second.size();
		}

		sort(typoGroupSorted.begin(), typoGroupSorted.end(), [](decltype(typoGroup)::pointer a, decltype(typoGroup)::pointer b)
			{
				return a->first < b->first;
			});

		ret.typoForms.reserve(totTfSize + 1);
		
		size_t estimatedNodeSize = 0;
		const KString* prevForm = nullptr;
		bool hash = false;
		for (auto f : typoGroupSorted)
		{
			ret.typoForms.insert(ret.typoForms.end(), f->second.begin(), f->second.end());
			for (auto it = ret.typoForms.end() - f->second.size(); it != ret.typoForms.end(); ++it)
			{
				it->typoId = ret.typoPtrs.size();
			}
			ret.typoPtrs.emplace_back(ret.typoPool.size());
			ret.typoPool += f->first;

			if (hash)
			{
				for (size_t i = 0; i < f->second.size(); ++i)
				{
					ret.typoForms.rbegin()[i].scoreHash = -ret.typoForms.rbegin()[i].scoreHash;
				}
			}

			if (!prevForm)
			{
				estimatedNodeSize += f->first.size();
				prevForm = &f->first;
				continue;
			}
			size_t commonPrefix = 0;
			while (commonPrefix < std::min(prevForm->size(), f->first.size())
				&& (*prevForm)[commonPrefix] == f->first[commonPrefix]) ++commonPrefix;
			estimatedNodeSize += f->first.size() - commonPrefix;
			prevForm = &f->first;
			hash = !hash;
		}
		ret.typoForms.emplace_back(0, 0, hash);
		ret.typoPtrs.emplace_back(ret.typoPool.size());
		formTrie.reserveMore(estimatedNodeSize);

		decltype(formTrie)::CacheStore<const KString> cache;
		size_t cumulated = 0;
		for (auto f : typoGroupSorted)
		{
			formTrie.buildWithCaching(f->first, reinterpret_cast<const Form*>(&ret.typoForms[cumulated]), cache);
			cumulated += f->second.size();
		}
	}

	static tp::Table<FnFreezeTrie, AvailableArch> table{ FreezeTrieGetter{} };
	auto* fn = table[static_cast<std::ptrdiff_t>(archType)];
	if (!fn) throw std::runtime_error{ std::string{"Unsupported architecture : "} + archToStr(archType)};
	ret.formTrie = (*fn)(move(formTrie));
	return ret;
}

inline bool testSpeicalChr(const u16string& form)
{
	POSTag pos;
	switch (pos = identifySpecialChr(form.back()))
	{
	case POSTag::sf:
	case POSTag::sp:
	case POSTag::ss:
	case POSTag::se:
	case POSTag::so:
	case POSTag::sw:
		return false;
	case POSTag::sl:
	case POSTag::sn:
	case POSTag::sh:
		if (all_of(form.begin(), form.end(), [&](char16_t c)
		{
			return pos == identifySpecialChr(c);
		}))
		{
			return false;
		}
	default:
		return true;
	}
}

vector<WordInfo> KiwiBuilder::extractWords(const U16MultipleReader& reader, size_t minCnt, size_t maxWordLen, float minScore, float posThreshold, bool lmFilter) const
{
	vector<WordInfo> cands = detector.extractWords(reader, minCnt, maxWordLen, minScore);
	if (!lmFilter) return cands;

	vector<WordInfo> ret;
	Kiwi kiwiInst = build();

	unordered_set<KString> allForms;
	for (auto& f : forms)
	{
		allForms.emplace(f.form);
	}

	for (auto& r : cands)
	{
		if (r.posScore[POSTag::nnp] < posThreshold || !r.posScore[POSTag::nnp]) continue;
		char16_t bracket = 0;
		switch (r.form.back())
		{
		case u')':
			if (r.form.find(u'(') == r.form.npos) continue;
			bracket = u'(';
			break;
		case u']':
			if (r.form.find(u'[') == r.form.npos) continue;
			bracket = u'[';
			break;
		case u'}':
			if (r.form.find(u'{') == r.form.npos) continue;
			bracket = u'{';
			break;
		case u'(':
		case u'[':
		case u'{':
			r.form.pop_back();
		default:
			if (r.form.find(u'(') != r.form.npos && r.form.find(u')') == r.form.npos)
			{
				bracket = u'(';
				goto onlyBracket;
			}
			else if (r.form.find(u'[') != r.form.npos && r.form.find(u']') == r.form.npos)
			{
				bracket = u'[';
				goto onlyBracket;
			}
			else if (r.form.find(u'{') != r.form.npos && r.form.find(u'}') == r.form.npos)
			{
				bracket = u'{';
				goto onlyBracket;
			}
			if (!testSpeicalChr(r.form)) continue;
		}

		{
			auto normForm = normalizeHangul(r.form);
			if (allForms.count(normForm)) continue;

			TokenResult kr = kiwiInst.analyze(r.form, Match::none);
			if (any_of(kr.first.begin(), kr.first.end(), [](const TokenInfo& kp)
			{
				return POSTag::jks <= kp.tag && kp.tag <= POSTag::etm;
			}) && kr.second >= -35)
			{
				continue;
			}

			allForms.emplace(normForm);
			ret.emplace_back(r);
		}
	onlyBracket:;
		if (bracket)
		{
			auto subForm = r.form.substr(0, r.form.find(bracket));
			if (subForm.empty()) continue;
			if (!testSpeicalChr(subForm)) continue;
			auto subNormForm = normalizeHangul(subForm);
			if (allForms.count(subNormForm)) continue;

			TokenResult kr = kiwiInst.analyze(subForm, Match::none);
			if (any_of(kr.first.begin(), kr.first.end(), [](const TokenInfo& kp)
			{
				return POSTag::jks <= kp.tag && kp.tag <= POSTag::etm;
			}) && kr.second >= -35)
			{
				continue;
			}

			allForms.emplace(subNormForm);
			ret.emplace_back(r);
			ret.back().form = subForm;
		}
	}
	return ret;
}

vector<WordInfo> KiwiBuilder::extractAddWords(const U16MultipleReader& reader, size_t minCnt, size_t maxWordLen, float minScore, float posThreshold, bool lmFilter)
{
	vector<WordInfo> words = extractWords(reader, minCnt, maxWordLen, minScore, posThreshold, lmFilter);
	for (auto& w : words)
	{
		addWord(w.form);
	}
	return words;
}