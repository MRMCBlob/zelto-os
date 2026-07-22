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
//   - A CORPUS-DERIVED LIST. The obvious source is a public-domain frequency list
//     (SCOWL / the Ubuntu `wamerican` package, or a Gutenberg-derived n-gram
//     count). The build environment has no network route, so it could not be
//     fetched and pinned in this phase; and neither of those two is what is
//     wanted anyway — SCOWL is a spelling wordlist with no frequencies and over
//     100k entries, and an n-gram count needs the same hand-editing at the head
//     to be usable. Written down as the upgrade: replace THIS FILE, and nothing
//     above it changes.
//   - APOSTROPHES. "don't", "it's", "we're" are among the hundred commonest
//     tokens in English and none of them can be in a trie over a-z. They are here
//     as "dont", "its", "were" — which is what a phone user types, because the
//     apostrophe is on the symbols layer — and the keyboard does NOT expand them.
//     Expansion is an autocorrect decision that changes a character the user did
//     not type into one they cannot see; it needs the suggestion strip to be
//     defensible, and the strip exists now, so this is a candidate for next
//     phase and not a thing the dictionary can fix.
//   - INFLECTIONS. "walk" is here; "walks", "walked", "walking" are not, unless
//     they are common enough to earn their own rank. A stemmer would multiply the
//     list by four for the same coverage, and a suffix rule would need the trie to
//     answer "is this a word" in two places instead of one.
//   - PROPER NOUNS AND NAMES. The trie is lower-case, and a name is exactly the
//     string P47's centre-zone rule exists to protect: the keyboard must type it
//     accurately without knowing it, and it does.
//   - A LEARNED USER DICTIONARY. It is the obvious next feature and it is a
//     privacy decision as much as a technical one (what is stored, where, and
//     whether a password field can leak into it). Not started here.
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

#endif  // ZELTO_KBD_WORDS_EN_H
