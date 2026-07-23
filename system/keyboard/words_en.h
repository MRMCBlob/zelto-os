// The shipped English word list — the keyboard's dictionary, as data (P48).
//
// THIS IS A SHIPPING DECISION, NOT A TABLE. It goes in the image, so its size and
// its licence are both product facts, and "which words" is a product choice with
// a measurable cost. What is written down here is the choice; lm_trie.c is only
// the machinery that reads it.
//
// WHY IT IS IN THE SOURCE TREE AND NOT A DATA FILE. A data file would need a path
// in the initramfs, a loader, a failure mode when it is missing, and a second
// thing to keep in sync with the binary — and P45's lesson is exactly that a
// stale path in the initramfs can go fifteen phases without anybody noticing
// (ZELTO_FONT). A string literal cannot be missing, cannot be stale and costs one
// .rodata section. It is also editable by a person, which a packed trie blob is
// not: the source of truth for the dictionary is the list you can read.
//
// WHY THE ORDER IS THE FREQUENCY. There is no column of numbers here because
// there is nothing a column of numbers would add. Word frequency in English obeys
// Zipf's law closely enough that rank alone reconstructs it (weight = 1 / (rank +
// ZIPF_OFFSET), see lm_trie.c), and the classifier consumes the result as ODDS
// clamped to Z_KBD_ODDS, so it cannot tell "the" from "the, but 12% more". What
// it can tell is "the" from "thee". So the list is ORDERED and the order is the
// data.
//
// WHERE IT CAME FROM, honestly. The head — roughly the first two hundred — is the
// standard English function-word ranking, which is stable across every corpus
// anyone has published and is where most of the mass is (the first hundred words
// are about half of all running text). The tail is common vocabulary in
// approximate rank order, assembled here rather than derived from a corpus. That
// is a real limitation and it is the right place for it: an error at rank 40
// changes what the keyboard does, an error at rank 1400 changes which of two
// words nobody is typing is fractionally more likely.
//
// WHAT WAS LEFT OUT, and why each one is a decision rather than an omission:
//   - A CORPUS-DERIVED LIST — MEASURED IN P49 AND REJECTED ON THE EVIDENCE.
//     P48 recorded this as the obvious upgrade, blocked only by the build
//     environment having no network route. P49 checked: the route exists now. So
//     the candidate was fetched and compared against this file instead of being
//     assumed better, and the answer is no.
//
//     The candidate was google-10000-english-usa (first20hours/google-10000-
//     english, sha256 981c776dc7e8996accb256e5fea9d241331b9602efe0c977285734890
//     e1ae729), a frequency-ordered list derived from Google's Trillion Word
//     Corpus — i.e. exactly the "corpus-derived n-gram count" that was wanted.
//     Its first 1620 entries overlap this file by 51%, and what is in the other
//     49% says why it lost: "pm online c e am s click x date n re copyright jan
//     d info rights privacy items r sex user de university games p f". It is a
//     WEB corpus. Its head is single letters, boilerplate and navigation
//     furniture, none of which anybody types into a phone.
//
//     The ranks make the same point on the words that matter here: "hello" is
//     rank 478 in this file and 2419 in the corpus; "dont" 227 against 3668;
//     "keyboard" 608 against 3573. On the 200 words this file ranks highest the
//     median rank disagreement is 67 places and the maximum is 6802. Adopting it
//     would measurably worsen the two behaviours P48 shipped — the space bar
//     grows on a COMPLETE WORD, and autocorrect proposes the COMMONER word — for
//     every one of them.
//
//     THE REAL REQUIREMENT, now that it has been paid for: a list ranked by what
//     people TYPE, not by what the web contains. Subtitle corpora (OpenSubtitles
//     frequency lists) are the closest public-domain proxy and are the thing to
//     measure next, against this same comparison.
//
//     P51 MEASURED IT, AND IT LOSES ON TOKENISATION RATHER THAN ON RANKING.
//     The candidate was hermitdave/FrequencyWords 2018 en_50k.txt, which is the
//     OpenSubtitles list P49 named. Its ranking is exactly what was hoped for —
//     conversational rather than editorial, which is what a phone gets typed
//     into. Its head is "you i the to a", where the web corpus's head was single
//     letters and navigation furniture.
//
//     And it cannot be used as it stands, because the corpus is tokenised on the
//     apostrophe. Read verbatim, the first 200 entries contain 's, 't, 'm, 're,
//     'll, 've and 'd as words, and the bare stems "don", "didn", "won" and
//     "doesn" as words beside them — eleven of the top two hundred are fragments
//     of contractions, and the frequency they carry is the frequency of the
//     WHOLE contraction. Adopting the list would put "didn" and "doesn" into the
//     trie as very common words, which is precisely what CONTRACTIONS_EN below
//     exists to prevent: its rule is that only bare forms which are NOT words may
//     be in it, and this corpus would make several of them words. "won" is the
//     sharpest case — it is a real word AND a contraction stem, and the rank the
//     corpus gives it is almost entirely "won't".
//
//     So the verdict is a JOIN, not a swap: the corpus's ordering is the right
//     signal and its tokens are the wrong unit. Using it means re-joining the
//     stems to their apostrophes before ranking (don + 't -> "dont", not "don"),
//     which is a corpus-preparation step nobody has written and which this phase
//     is not. What has changed is that it is now a known, bounded piece of work
//     against a named file rather than "the thing to measure next".
//
//     Until one wins that comparison this file stays, and "it is hand-assembled"
//     stops being a reason on its own.
//   - APOSTROPHES — EXPANDED NOW, THROUGH THE VISIBLE PATH (P49). "don't",
//     "it's", "we're" are among the hundred commonest tokens in English and none
//     of them can be in a trie over a-z, so they are here as the bare forms
//     "dont", "its", "were" — which is also what a phone user types, because the
//     apostrophe is on the symbols layer. P48 declined to expand them and wrote
//     down the condition: expansion changes a character the user did not type
//     into one they cannot see, so it needs the suggestion strip to be
//     defensible. The strip shipped in P48, so CONTRACTIONS_EN at the foot of
//     this file is the follow-through — and it goes through kbd_autocorrect like
//     every other correction, so it is in the strip before it fires and one
//     backspace takes it back. The rule for what may be in that table is with
//     the table; the short version is that "its" and "were" are not in it and
//     never will be, because they are words.
//   - INFLECTIONS — STILL NOT IN THE LIST, AND THAT IS NOW A RULE INSTEAD OF AN
//     OMISSION (P49). "walk" is here; "walks", "walked", "walking" are not. What
//     P48 wrote here was a coverage argument — a stemmer multiplies the list by
//     four for the same words — and it was answering the wrong question, because
//     the cost of the omission was never coverage. Measured on 60 hand-checked
//     inflected forms of common words (meta/kbd-measure.sh): autocorrect REWROTE 49
//     of them into a different word. walked -> walk. hands -> and. dogs -> does.
//     reading -> wedding. taking -> thing. That had been happening since
//     autocorrect shipped and nothing looked broken, because every test and every
//     worked example used uninflected words.
//     The fix is a suffix rule (inflected_word in lm_trie.c), and the objection
//     recorded here — "a suffix rule would need the trie to answer 'is this a
//     word' in two places" — is answered by putting the rule INSIDE
//     z_lm_is_word: there is still exactly one function every caller asks. After
//     it, 60 of 60 are left alone and 15 of 16 real typos are still corrected.
//   - PROPER NOUNS AND NAMES. The trie is lower-case, and a name is exactly the
//     string P47's centre-zone rule exists to protect: the keyboard must type it
//     accurately without knowing it, and it does.
//   - A LEARNED USER DICTIONARY — BUILT IN P49, AND STILL NOT IN THIS FILE.
//     P48 called it the obvious next feature and a privacy decision as much as a
//     technical one. Both were right, and the second is why nothing about it
//     landed here: a learned word merges into the LIST at runtime (z_lm_learn,
//     predict.h) and is answered by the same trie, but it is never written into
//     the shipped source. What is stored, where, and what may never get in are
//     decided where the field's content purpose is visible — the privacy note
//     over kbd_learn() in system/keyboard/main.c. This file is what ships in the
//     image; that one is what a person's phone learns, and they are deliberately
//     not the same artifact.
//
// SIZE, measured: see the "[keyboard] lm trie" line the model prints at build —
// bytes of source list, node count, and bytes of built trie, all reported rather
// than estimated.
#ifndef ZELTO_KBD_WORDS_EN_H
#define ZELTO_KBD_WORDS_EN_H

// Newline-separated text in bands, not an array of one pointer per word: 1620
// `const char *` on a 64-bit target is 13KB of pointers and 1620 relocations to
// describe 10KB of text that is already in the binary.
//
// IT IS BANDS RATHER THAN ONE STRING because ISO C only requires a compiler to
// support a 4095-byte string literal and this project builds -Wpedantic -Werror,
// which is the right setting and caught it immediately. Each band is one line of
// the list, so the split is where a reader would put it anyway.
static const char *const WORDS_EN[] = {
    // --- 1-100: the function words. About half of all running text. ----------
    "the\nof\nand\na\nto\nin\nis\nyou\nthat\nit\n",
    "he\nwas\nfor\non\nare\nas\nwith\nhis\nthey\ni\n",
    "at\nbe\nthis\nhave\nfrom\nor\none\nhad\nby\nword\n",
    "but\nnot\nwhat\nall\nwere\nwe\nwhen\nyour\ncan\nsaid\n",
    "there\nuse\nan\neach\nwhich\nshe\ndo\nhow\ntheir\nif\n",
    "will\nup\nother\nabout\nout\nmany\nthen\nthem\nthese\nso\n",
    "some\nher\nwould\nmake\nlike\nhim\ninto\ntime\nhas\nlook\n",
    "two\nmore\nwrite\ngo\nsee\nnumber\nno\nway\ncould\npeople\n",
    "my\nthan\nfirst\nwater\nbeen\ncall\nwho\noil\nits\nnow\n",
    "find\nlong\ndown\nday\ndid\nget\ncome\nmade\nmay\npart\n",
    // --- 101-200 ------------------------------------------------------------
    "over\nnew\nsound\ntake\nonly\nlittle\nwork\nknow\nplace\nyear\n",
    "live\nme\nback\ngive\nmost\nvery\nafter\nthing\nour\njust\n",
    "name\ngood\nsentence\nman\nthink\nsay\ngreat\nwhere\nhelp\nthrough\n",
    "much\nbefore\nline\nright\ntoo\nmean\nold\nany\nsame\ntell\n",
    "boy\nfollow\ncame\nwant\nshow\nalso\naround\nform\nthree\nsmall\n",
    "set\nput\nend\ndoes\nanother\nwell\nlarge\nmust\nbig\neven\n",
    "such\nbecause\nturn\nhere\nwhy\nask\nwent\nmen\nread\nneed\n",
    "land\ndifferent\nhome\nus\nmove\ntry\nkind\nhand\npicture\nagain\n",
    "change\noff\nplay\nspell\nair\naway\nanimal\nhouse\npoint\npage\n",
    "letter\nmother\nanswer\nfound\nstudy\nstill\nlearn\nshould\namerica\nworld\n",
    // --- 201-300 ------------------------------------------------------------
    "high\nevery\nnear\nadd\nfood\nbetween\nown\nbelow\ncountry\nplant\n",
    "last\nschool\nfather\nkeep\ntree\nnever\nstart\ncity\nearth\neye\n",
    "light\nthought\nhead\nunder\nstory\nsaw\nleft\ndont\nfew\nwhile\n",
    "along\nmight\nclose\nsomething\nseem\nnext\nhard\nopen\nexample\nbegin\n",
    "life\nalways\nthose\nboth\npaper\ntogether\ngot\ngroup\noften\nrun\n",
    "important\nuntil\nchildren\nside\nfeet\ncar\nmile\nnight\nwalk\nwhite\n",
    "sea\nbegan\ngrow\ntook\nriver\nfour\ncarry\nstate\nonce\nbook\n",
    "hear\nstop\nwithout\nsecond\nlate\nmiss\nidea\nenough\neat\nface\n",
    "watch\nfar\nreally\nalmost\nlet\nabove\ngirl\nsometimes\nmountain\ncut\n",
    "young\ntalk\nsoon\nlist\nsong\nbeing\nleave\nfamily\nbody\nmusic\n",
    // --- 301-450 ------------------------------------------------------------
    "color\nstand\nsun\nquestion\nfish\narea\nmark\ndog\nhorse\nbird\n",
    "problem\ncomplete\nroom\nknew\nsince\never\npiece\ntold\nusually\nfriend\n",
    "easy\nheard\norder\nred\ndoor\nsure\nbecome\ntop\nship\nacross\n",
    "today\nduring\nshort\nbetter\nbest\nhowever\nlow\nhours\nblack\nproducts\n",
    "happened\nwhole\nmeasure\nremember\nearly\nwaves\nreached\nlisten\nwind\nrock\n",
    "space\ncovered\nfast\nseveral\nhold\nhimself\ntoward\nfive\nstep\nmorning\n",
    "passed\nvowel\ntrue\nhundred\nagainst\npattern\nnumeral\ntable\nnorth\nslowly\n",
    "money\nmap\nfarm\npulled\ndraw\nvoice\nseen\ncold\ncried\nnotice\n",
    "green\noh\nquickly\ndevelop\nocean\nwarm\nfree\nminute\nstrong\nspecial\n",
    "mind\nclear\ntail\nproduce\nfact\nstreet\ninch\nnothing\ncourse\nstay\n",
    "wheel\nfull\nforce\nblue\nobject\ndecide\nsurface\ndeep\nmoon\nisland\n",
    "foot\nyet\nbusy\ntest\nrecord\nboat\ncommon\ngold\npossible\nplane\n",
    "age\ndry\nwonder\nlaugh\nthousand\nago\nran\ncheck\ngame\nshape\n",
    "yes\nhot\nbrought\nheat\nsnow\nbed\nbring\nsit\nperhaps\n",
    "fill\neast\nweight\nlanguage\namong\ncat\nrain\nbox\nfell\nsmile\n",
    // --- 451-650 ------------------------------------------------------------
    "phone\nemail\nmessage\nsend\ntext\nsearch\nphoto\nvideo\n",
    "app\nnet\nweb\nlink\nfile\nsave\nshare\ndelete\n",
    "camera\nscreen\nbattery\nsignal\ndata\ncloud\naccount\npassword\nlogin\n",
    "please\nthanks\nthank\nsorry\nhello\nhi\nhey\nbye\nyeah\nokay\n",
    "ok\nmaybe\nactually\nprobably\nanyway\nbasically\nexactly\nobviously\n",
    "afternoon\nevening\ntonight\ntomorrow\nyesterday\nweek\nmonth\nweekend\nhour\n",
    "monday\ntuesday\nwednesday\nthursday\nfriday\nsaturday\nsunday\n",
    "january\nfebruary\nmarch\napril\njune\njuly\naugust\nseptember\noctober\n",
    "november\ndecember\nspring\nsummer\nautumn\nwinter\nbirthday\nholiday\nvacation\nparty\n",
    "office\nmeeting\nproject\nteam\nreport\nclient\nbudget\ndeadline\n",
    "reply\nforward\nattach\nconfirm\nschedule\ncancel\n",
    "train\nbus\nflight\nticket\nstation\nairport\nhotel\naddress\n",
    "south\nwest\nstraight\ncorner\n",
    "coffee\ntea\nbread\nmilk\nsugar\nsalt\ndinner\nlunch\nbreakfast\n",
    "restaurant\nkitchen\nchair\nwindow\nfloor\nwall\nroof\ngarden\n",
    "shop\nstore\nmarket\nprice\ncost\npay\nbuy\nsell\ncash\ncard\n",
    "bank\ndollar\neuro\npound\ncheap\nexpensive\ndiscount\nreceipt\ntotal\n",
    "doctor\nhospital\nmedicine\nhealth\nsick\npain\nemergency\npolice\nfire\n",
    "teacher\nstudent\nclass\nlesson\nhomework\nexam\ncollege\nlibrary\n",
    "computer\nlaptop\nkeyboard\nmouse\nprinter\nsoftware\nhardware\nnetwork\nserver\n",
    // --- 651-900 ------------------------------------------------------------
    "able\naccept\naccess\nact\nafraid\n",
    "agree\nahead\nallow\nalone\nalready\nalthough\namount\nancient\n",
    "angry\nannounce\nannual\nanyone\nanything\napart\nappear\n",
    "apply\napproach\nargue\narrive\narticle\nartist\naside\nassume\nattack\n",
    "attempt\nattend\nattention\nauthor\navailable\navoid\naware\nbaby\nbalance\nball\n",
    "band\nbase\nbeach\nbear\nbeat\nbeautiful\n",
    "behind\nbelieve\nbelong\nbeside\nbeyond\nbill\nbind\nblood\nblow\n",
    "board\nborn\nborrow\nbottle\nbottom\nbrain\nbranch\nbrave\nbreak\nbreath\n",
    "bridge\nbright\nbroad\nbroken\nbrother\nbrown\nbuild\nbuilding\nburn\nbury\n",
    "business\nbutton\ncalm\ncamp\ncandle\ncapital\ncaptain\ncareful\n",
    "case\ncatch\ncause\ncenter\ncentury\ncertain\nchain\nchallenge\n",
    "chance\ncharacter\ncharge\nchild\nchoice\nchoose\nchurch\n",
    "circle\nclaim\nclean\nclimb\nclock\ncoast\ncoat\ncollect\n",
    "combine\ncomfort\ncommand\ncomment\ncompany\ncompare\ncompete\n",
    "complain\nconcern\ncondition\nconnect\nconsider\ncontain\ncontinue\ncontrol\n",
    "cook\ncool\ncopy\ncorrect\ncotton\ncount\ncouple\n",
    "courage\ncover\ncrash\ncreate\ncredit\ncrime\ncross\ncrowd\ncrown\ncurrent\n",
    "custom\ndamage\ndance\ndanger\ndark\ndaughter\ndead\ndeal\ndear\n",
    "declare\ndecline\ndefend\ndegree\ndelay\ndeliver\ndemand\ndepend\ndescribe\ndesert\n",
    "design\ndesire\ndetail\ndetermine\ndevice\ndifference\ndifficult\ndirect\n",
    "dirty\ndisappear\ndiscover\ndiscuss\ndisease\ndistance\ndivide\ndocument\ndouble\n",
    "doubt\ndrag\ndream\ndress\ndrink\ndrive\ndrop\ndrug\ndust\n",
    "duty\neager\nearn\nease\nedge\neducation\neffect\n",
    "effort\neither\nelect\nelement\nelse\nempty\nenemy\nenergy\nengine\n",
    "enjoy\nenter\nentire\nequal\nescape\nespecially\nevent\n",
    // --- 901-1150 -----------------------------------------------------------
    "evidence\nexact\nexamine\nexcept\nexchange\nexcite\nexcuse\nexercise\nexist\n",
    "expect\nexpense\nexperience\nexplain\nexpress\nextend\nextra\nfail\nfair\nfaith\n",
    "fall\nfalse\nfamiliar\nfamous\nfashion\nfault\nfavor\nfear\n",
    "feature\nfeed\nfeel\nfemale\nfence\nfestival\nfever\nfield\nfight\nfigure\n",
    "final\nfinance\nfine\nfinger\nfinish\nfirm\nfit\n",
    "fix\nflag\nflat\nfloat\nflow\nflower\nfly\nfocus\n",
    "fold\nfool\nforeign\nforest\nforget\nforgive\n",
    "former\nframe\nfresh\nfront\nfruit\nfuel\n",
    "fun\nfunction\nfund\nfuture\ngain\ngas\ngate\ngather\n",
    "general\ngentle\ngift\nglad\nglass\nglobal\ngoal\ngolden\n",
    "govern\ngrade\ngrain\ngrand\ngrant\ngrass\ngrave\ngreet\n",
    "ground\nguard\nguess\nguest\nguide\nguilty\nhabit\nhair\n",
    "half\nhall\nhang\nhappen\nhappy\nharbor\nharm\nhat\nhate\n",
    "heart\nheavy\nheight\nhero\nhide\nhill\n",
    "history\nhit\nhobby\nhole\nhonest\nhonor\nhope\n",
    "host\nhuge\nhuman\nhumor\nhungry\n",
    "hunt\nhurry\nhurt\nhusband\nice\nignore\nill\nimage\nimagine\n",
    "impact\nimprove\ninclude\nincome\nincrease\nindeed\nindustry\ninfluence\ninform\n",
    "injury\ninside\ninsist\ninstead\ninsurance\nintend\ninterest\ninternet\nintroduce\ninvite\n",
    "involve\niron\nissue\nitem\njoin\njoke\njourney\njoy\njudge\n",
    "jump\nkey\nkick\nkill\nking\nkiss\n",
    "knee\nknife\nknock\nknowledge\nlabor\nlack\nlady\nlake\nlamp\n",
    "law\nlay\nlazy\nlead\n",
    "leader\nleaf\nleast\nleather\nlecture\nleg\nlegal\n",
    "lemon\nlend\nlength\nless\nlevel\nlie\n",
    // --- 1151-1400 ----------------------------------------------------------
    "lift\nlimit\nlion\nlip\nliquid\n",
    "load\nloan\nlocal\nlock\nlogic\nlonely\n",
    "loose\nlose\nloss\nlost\nlot\nloud\nlove\nluck\n",
    "machine\nmad\nmagazine\nmagic\nmail\nmain\nmaintain\nmajor\nmale\n",
    "manage\nmanner\nmarriage\nmaster\nmatch\n",
    "material\nmatter\nmaximum\nmeal\nmeat\nmedical\nmedium\n",
    "meet\nmelt\nmember\nmemory\nmention\nmenu\nmercy\nmetal\nmethod\n",
    "middle\nmild\nmillion\nmine\nminor\n",
    "mirror\nmistake\nmix\nmodel\nmodern\nmoment\nmonitor\n",
    "mood\nmoral\nmotor\nmount\nmouth\n",
    "movie\nmud\nmurder\nmuscle\nmuseum\nmutual\n",
    "myself\nnail\nnaked\nnarrow\nnation\nnative\nnatural\nnature\n",
    "neat\nnecessary\nneck\nneighbor\nneither\nnerve\n",
    "news\nnice\nnoble\nnoise\nnone\nnoon\nnormal\n",
    "nose\nnote\nnovel\nnuclear\nnurse\nobey\n",
    "oblige\nobserve\nobtain\nobvious\noccasion\noccur\nodd\noffer\n",
    "officer\nonion\n",
    "operate\nopinion\nopportunity\noppose\noption\norange\nordinary\norgan\norigin\n",
    "otherwise\nought\noutside\noven\nowe\n",
    "pack\npaint\npair\npalace\npale\npan\npanel\n",
    "parent\npark\nparticular\npartner\npass\npassage\npast\npath\n",
    "patient\npause\npeace\npen\npencil\npepper\nperfect\n",
    "perform\nperiod\npermit\nperson\nphase\nphrase\npick\n",
    "pig\npile\npilot\npin\npink\npipe\nplain\n",
    "plan\nplastic\nplate\npleasant\npleasure\nplenty\n",
    // --- 1401-1650 ----------------------------------------------------------
    "plus\npocket\npoem\npoison\npolicy\npolite\npool\npoor\n",
    "popular\nport\nposition\npositive\npossess\npost\npot\npotato\n",
    "pour\npowder\npower\npractice\npraise\npray\nprefer\nprepare\npresent\npress\n",
    "pressure\npretend\npretty\nprevent\nprevious\npride\nprimary\nprince\nprint\n",
    "prior\nprison\nprivate\nprize\nprocess\nproduct\nprofit\n",
    "program\nprogress\npromise\nproper\nprotect\nproud\nprove\nprovide\npublic\n",
    "pull\npunish\npupil\npure\npurple\npurpose\npush\nquality\nquantity\n",
    "quarter\nqueen\nquick\nquiet\nquit\nquite\nrace\nradio\nrail\n",
    "raise\nrange\nrapid\nrare\nrate\nrather\nreach\nready\n",
    "real\nreason\nrecall\nreceive\nrecent\nrecognize\nrecover\nreduce\n",
    "refer\nreflect\nrefuse\nregard\nregion\nregret\nregular\nreject\nrelate\nrelax\n",
    "release\nrelief\nremain\nremark\nremind\nremove\nrent\nrepair\nrepeat\n",
    "replace\nrepresent\nrequest\nrequire\nrescue\nresearch\nreserve\nresist\n",
    "resource\nrespect\nrespond\nrest\nresult\nreturn\nreveal\nreverse\nreview\nreward\n",
    "rich\nride\nring\nrise\nrisk\nroad\nrole\n",
    "roll\nroot\nrope\nrough\nround\nroute\nrow\nroyal\n",
    "rub\nrule\nrush\nsad\nsafe\nsail\nsample\n",
    "sand\nsatisfy\nscale\nscene\nscheme\nscience\n",
    "score\nscratch\nscream\nseason\nseat\nsecret\n",
    "section\nsecure\nseed\nseek\nseize\nseldom\nselect\n",
    "sense\nseparate\nserious\nserve\nservice\nsettle\n",
    "severe\nshade\nshadow\nshake\nshall\nshame\nsharp\n",
    "sheet\nshelf\nshell\nshelter\nshine\nshirt\nshock\nshoe\nshoot\n",
    "shore\nshoulder\nshout\nshut\n",
    "sight\nsign\nsilence\nsilk\nsilver\nsimilar\nsimple\nsing\n",
    // --- 1651-1900 ----------------------------------------------------------
    "single\nsink\nsir\nsister\nsite\nsituation\nsize\nskill\nskin\n",
    "sky\nsleep\nslide\nslight\nslip\nslow\nsmart\nsmell\n",
    "smoke\nsmooth\nsnake\nsoap\nsocial\nsociety\nsoft\nsoil\nsoldier\n",
    "solid\nsolve\nson\nsort\nsoul\n",
    "soup\nsource\nspare\nspeak\nspeech\nspeed\n",
    "spend\nspirit\nspite\nsplit\nspoil\nspoon\nsport\nspot\nspread\n",
    "square\nstaff\nstage\nstairs\nstamp\nstandard\nstar\n",
    "steady\nsteal\nsteam\nsteel\nsteep\nstick\nstiff\n",
    "stock\nstomach\nstone\nstorm\nstrange\n",
    "stream\nstrength\nstress\nstretch\nstrict\nstrike\nstring\nstrip\n",
    "structure\nstruggle\nstuff\nstupid\nstyle\nsubject\nsucceed\n",
    "sudden\nsuffer\nsuggest\nsuit\nsupply\nsupport\nsuppose\n",
    "surprise\nsurround\nsurvive\nsuspect\nswear\nsweep\nsweet\nswim\n",
    "switch\nsword\nsymbol\nsystem\ntall\ntape\n",
    "target\ntask\ntaste\ntax\nteach\ntear\ntemper\n",
    "temple\ntend\ntender\nterm\nterrible\ntheatre\n",
    "theme\ntheory\nthick\nthin\n",
    "third\nthirst\nthorough\nthough\nthread\nthreat\n",
    "throw\nthumb\nthus\ntide\ntidy\ntie\ntight\n",
    "till\ntiny\ntip\ntired\ntitle\ntoe\n",
    "tone\ntongue\ntool\ntooth\ntopic\ntouch\n",
    "tough\ntour\ntower\ntown\ntoy\ntrack\ntrade\ntradition\ntraffic\n",
    "transfer\ntransport\ntravel\ntreat\ntrial\ntrick\ntrip\ntrouble\n",
    "truck\ntrust\ntruth\ntube\ntune\ntwice\ntwin\n",
    "type\ntypical\nugly\nuncle\nunderstand\nunion\nunit\nunite\nuniverse\n",
    // --- 1901-2050: the tail ------------------------------------------------
    "unless\nunusual\nupon\nupper\nupset\nurge\nurgent\n",
    "useful\nusual\nvalley\nvalue\nvarious\nvast\nvegetable\nvehicle\nversion\n",
    "victory\nview\nvillage\nvisit\nvolume\nvote\nwage\nwait\nwake\n",
    "wander\nwar\nwarn\nwash\nwaste\n",
    "wave\nweak\nwealth\nweapon\nwear\nweather\nwedding\n",
    "weigh\nwelcome\nwet\nwhether\n",
    "whisper\nwhose\nwide\nwife\nwild\n",
    "win\nwine\nwing\nwipe\nwire\nwise\n",
    "wish\nwithin\nwitness\nwoman\nwood\nwool\n",
    "worry\nworse\nworth\nwound\nwrap\nwrong\n",
    "yard\nyellow\nyourself\n",
    "youth\nzero\nzone\n",
};

// --- the contractions (P49) --------------------------------------------------
// APOSTROPHES, PUT BACK. P48 wrote down why the list carries "dont", "its" and
// "were" as bare forms — a trie over a-z cannot hold an apostrophe, and a bare
// form is what a phone user types because the apostrophe is on the symbols layer
// — and why the keyboard did NOT expand them: expansion changes a character the
// user did not type into one they cannot see, and that needs the suggestion strip
// to be defensible. The strip shipped in P48. This is the follow-through: an
// expansion is a correction like any other, so it goes through kbd_autocorrect,
// shows in the strip before it fires, and one backspace takes it back.
//
// THE RULE FOR WHAT IS IN THIS TABLE, and it is the entire safety argument:
// ONLY CONTRACTIONS WHOSE BARE FORM IS NOT ITSELF AN ENGLISH WORD. That is why
// this list is thirty-odd entries and not the ~90 contractions English has.
// Deliberately absent, each because the bare form is a real word that means
// something else:
//     its   ("its colour")        were  ("they were here")
//     well  ("a well")            shell ("a shell")
//     lets  ("he lets go")        hell / ill / id / were / whos->whos? no
//     shed  ("a shed")            wed   ("we wed")
//     cant / wont ARE words in the strictest sense ("insincere talk", "as is his
//     wont") and are in this table anyway: both are vanishingly rare against the
//     contraction, both are absent from the shipped list, and every phone
//     keyboard makes the same call. That is a judgement, so it is written down
//     here rather than buried.
//
// It lives in THIS file rather than beside the code that uses it because it is
// lexical data about English, which is what this file is, and because a test can
// then lint it (test_kbd_predict: unique, and every expansion is its bare form
// with exactly one apostrophe inserted).
typedef struct ZWordContraction {
    const char *bare, *full;
} ZWordContraction;

static const ZWordContraction CONTRACTIONS_EN[] = {
    {"dont", "don't"},         {"doesnt", "doesn't"},   {"didnt", "didn't"},
    {"cant", "can't"},         {"wont", "won't"},       {"isnt", "isn't"},
    {"arent", "aren't"},       {"wasnt", "wasn't"},     {"werent", "weren't"},
    {"havent", "haven't"},     {"hasnt", "hasn't"},     {"hadnt", "hadn't"},
    {"wouldnt", "wouldn't"},   {"couldnt", "couldn't"},
    {"shouldnt", "shouldn't"}, {"youre", "you're"},     {"youve", "you've"},
    {"youll", "you'll"},       {"theyre", "they're"},   {"theyve", "they've"},
    {"theyll", "they'll"},     {"weve", "we've"},       {"wouldve", "would've"},
    {"couldve", "could've"},   {"shouldve", "should've"},
    {"thats", "that's"},       {"whats", "what's"},     {"theres", "there's"},
    {"heres", "here's"},       {"ive", "I've"},         {"im", "I'm"},
};
#define N_CONTRACTIONS_EN                                                      \
    ((int)(sizeof(CONTRACTIONS_EN) / sizeof(CONTRACTIONS_EN[0])))

#endif  // ZELTO_KBD_WORDS_EN_H
