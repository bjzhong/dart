#include "ecfg/ecfgmain.h"
#include "ecfg/ecfgsexpr.h"
#include "util/vector_output.h"
#include "irrev/irrev_em_matrix.h"
#include "hsm/branch_length_em.h"

// maximum length for lines in Stockholm input alignments
#define MAX_LINE_LEN 12345678

// branch length resolution & max value
#define BRANCH_LENGTH_RES TINY
#define BRANCH_LENGTH_MAX 10.

// Stockholm tags for posterior probabilities
#define CONFIDENCE_TAG_PREFIX  "PC"
#define LOGPOSTPROB_TAG_PREFIX "PP"

// GFF tags for posterior probabilities
#define CYK_STATE_LABEL "CYK"  /* indicates that GFF postprob line is part of CYK trace */

// tags for training meta-information in grammar
#define TRAINING_INFO  "training-info"
#define TRAINING_TIME  "unix-time"
#define TRAINING_BITS  "final-bits"
#define ALIGN_FILENAME "alignment-filename"
#define ALIGN_STDIN    "<stdin>"

// ECFG_main
ECFG_main::ECFG_main (int argc, char** argv)
  : opts (argc, argv), alph (0), tree_estimation_chain (0),
    max_subseq_len (0)
{ }

void ECFG_main::add_grammar (const char* name, ECFG_scores* ecfg)
{
  ecfg_map[sstring (name)] = ecfg;
  ecfg->name = name;
  grammar_list.push_back (name);
}

void ECFG_main::init_opts (const char* desc)
{
  INIT_CONSTRUCTED_OPTS_LIST (opts, -1, "[options] [<alignment database(s) in Stockholm format>]", desc);

  opts.print_title ("Model selection");

  opts.add ("g -grammar", grammars_filename, "load grammar from file", false);

  if (!(preset = default_grammars).size())
    preset = grammar_list.front();
  sstring preset_help;
  preset_help << "use one of the presets: " << sstring::join (grammar_list, ",");
  if (grammar_list.size() > 1)
    opts.add ("p -preset", preset, preset_help.c_str());

  opts.add ("x -expand", dump_expanded = "", "dump macro-expanded grammar to file (prior to any training)", false);

  opts.add ("e -tree", tree_grammar_filename = "", "load separate tree-estimation grammar from file (point substitution models only)", false);
  opts.add ("l -length", max_subseq_len = -1, "limit maximum length of infix subseqs (context-free grammars only)", false);

  sstring gap_chars_help;
  gap_chars_help << "set character(s) that are used to denote gaps in alignment (default is \"" << DEFAULT_GAP_CHARS << "\")";
  opts.add ("gc -gapchar", gap_chars = "", gap_chars_help.c_str(), false);

  opts.newline();
  opts.print_title ("Algorithm selection");

  opts.add ("t -train", train, "train model by EM, saving grammar to file", false);
  opts.add ("a -annotate", annotate = true, "display & report log-likelihood of maximum-likelihood parse tree (CYK algorithm)");
  opts.add ("s -score", report_sumscore = false, "report log-likelihood, summed over all parse trees (Inside algorithm)");
  opts.add ("c -confidence", report_confidence = false, "report posterior log-probabilities of nodes in CYK parse tree");
  opts.add ("pp -postprob", report_postprob = false, "report posterior log-probabilities of all possible parse tree nodes");
  opts.add ("hc -hidden-classes", report_hidden_classes = false, "impute ML hidden classes at each site (for substitution models with hidden classes)");
  opts.add ("ar -ancrec-cyk-map", ancrec_CYK_MAP = false, "reconstruct Maximum-A-Posteriori ancestral sequence, conditional on CYK parse tree");
  opts.add ("arpp -ancrec-postprob", ancrec_postprob = false, "report P(ancestral residues | observed residues, phylo-alignment, parse tree)");

  opts.newline();
  opts.print_title ("EM convergence criteria");

  opts.add ("mr -maxrounds", em_max_iter = -1, "max number of \"rounds\" (iterations) of EM", false);
  opts.add ("mi -mininc", em_min_inc = .001, "minimum fractional increase in log-likelihood per round of EM");
  opts.add ("f -forgive", em_forgive = 0, "number of consecutive non-increasing rounds of EM to \"forgive\" before stopping");

  opts.newline();
  opts.print_title ("Pseudocounts");
  opts.print ("(Pseudocounts are obsolete; pseudocount tags are cooler! See biowiki.org/XrateFormat)\n\n");
  opts.add ("pi -pseudinitial", pseud_init = 1e-9, "pseudocount for initial state occupancies and probability parameters");
  opts.add ("pm -pseudmutate", pseud_mutate = 0., "pseudocount for mutation rates and rate parameters");
  opts.add ("pt -pseudtime", pseud_wait = 1e-4, "pseudo-wait time for mutation rates and rate parameters");

  opts.add ("bmin -branch-min", min_branch_len = .0001, "minimum branch length in phylogenies... not strictly a pseudocount");
  opts.add ("bres -branch-resolution", tres = .0001, "resolution of branch lengths in phylogenies... not strictly a pseudocount");

  opts.newline();
  opts.print_title ("Output");
  opts.add ("gff", gff_filename, "save GFF annotations to file, rather than interleaving into Stockholm output", false);
}

void ECFG_main::annotate_loglike (Stockholm& stock, const char* tag, const sstring& ecfg_name, Loge loglike) const
{
  sstring score_tag, score_string;
  score_tag << tag << "_" << ecfg_name;
  score_string << Nats2Bits (loglike);
  stock.add_gf_annot (score_tag, score_string);
}

void ECFG_main::run()
{
  // parse the command line
  try
    {
      if (!opts.parse()) { cerr << opts.short_help(); exit(1); }
    }
  catch (const Dart_exception& e)
    {
      cerr << opts.short_help();
      cerr << e.what();
      exit(1);
    }

  try
    {
      // set up the gap characters
      if (gap_chars.size())
	Alignment::set_gap_chars (gap_chars);

      // initialise Stockholm_database
      Sequence_database seq_db;
      Stockholm_database stock_db;
      vector<sstring> training_alignment_filename;
      if (!opts.args.size())
	{
	  // no alignment filenames specified; read from stdin
	  CLOGERR << "[waiting for alignments on standard input]\n";
	  stock_db.read (cin, seq_db, MAX_LINE_LEN);
	  training_alignment_filename.push_back (sstring (ALIGN_STDIN));
	}
      else
	for_const_contents (vector<sstring>, opts.args, align_db_filename)
	{
	  ifstream align_db_in ((const char*) align_db_filename->c_str());
	  if (!align_db_in) THROWEXPR ("Couldn't open alignment file '" << *align_db_filename << "'");
	  stock_db.read (align_db_in, seq_db, MAX_LINE_LEN);
	  training_alignment_filename.push_back (*align_db_filename);
	}

      // initialise Tree_alignment_database
      EM_tree_alignment_database align_db (seq_db);
      align_db.initialise_from_Stockholm_database (stock_db, FALSE);


      // HACK REPORT
      // Score_profile's are needed by the Tree_alignment tree estimation routines
      // (neighbor joining & branch-length EM).
      // thus, if any trees are missing, or if we are optimising branch lengths,
      // we first convert sequences to Score_profile's.
      // this is hacky, but appeases the conflicting needs of keeping memory low (for genomic seqs)
      // and being able to supply alignments without trees (for protein seqs and small nucleotide alignments).
      //
      // ADDENDUM: July 10, 2006; IH. Situation getting hackier.
      // Since the rate matrix for tree estimation is now in a separate grammar file,
      // it's conceivable that the Score_profile's could end up getting converted to the "wrong" alphabet here.
      // That is, it'll be the right Alphabet for tree estimation,
      // but wrong for any later operations involving the main grammar.
      // Solution: call seq_db.clear_scores() after tree estimation.
      bool missing_trees = false;
      for_contents (list<Tree_alignment>, align_db.tree_align_list, ta)
	if (ta->tree.nodes() == 0)
	  { missing_trees = true; break; }

      if (tree_grammar_filename.size() == 0 && missing_trees)
	{
	  THROWEXPR ("Every input alignment needs to be annotated with a phylogenetic tree.\n"
		     << "Given that this isn't the case, I need a rate matrix in order to do some ad-hoc phylogeny\n"
		     << " (neighbor-joining followed by EM on the branch lengths, if you must know.)\n"
		     << "I want to get that rate matrix from the tree estimation grammar file.\n"
		     << "However, I haven't been told about any such file, so I'm bailing.\n");
	}

      if (tree_grammar_filename.size())
	{
	  // set the matrix used for tree estimation to be the first single-character matrix in any grammar in the tree grammar file
	  Empty_alphabet tree_estimation_grammar_alphabet;
	  vector<ECFG_scores*> tree_estimation_grammars;
	  bool found_matrix = false;
	  if (tree_grammar_filename.size())
	    {
	      ECFG_builder::load_xgram_alphabet_and_grammars (tree_grammar_filename, tree_estimation_grammar_alphabet, tree_estimation_grammars);
	      for (int g = 0; !found_matrix && g < (int) tree_estimation_grammars.size(); ++g)
		for (int m = 0; !found_matrix && m < (int) tree_estimation_grammars[g]->matrix_set.chain.size(); ++m)
		  if (tree_estimation_grammars[g]->matrix_set.chain[m].word_len == 1)
		    {
		      tree_estimation_chain = &tree_estimation_grammars[g]->matrix_set.chain[m];
		      found_matrix = true;
		    }
	    }
	  if (missing_trees && !found_matrix)
	    THROWEXPR ("Every input alignment needs to be annotated with a phylogenetic tree.\n"
		       << "Given that this isn't the case, I need a rate matrix in order to do some ad-hoc phylogeny\n"
		       << " (neighbor-joining followed by EM on the branch lengths, if you must know.)\n"
		       << "I want to get that rate matrix from the tree estimation grammar file,\n"
		       << " in the form of a single-terminal 'chain'.\n"
		       << "However, I wasn't able to find any such chain in that file, so I'm bailing.\n");

	  // convert sequences to Score_profile's for tree estimation
	  Empty_alphabet tree_estimation_hidden_alphabet;
	  tree_estimation_hidden_alphabet.init_hidden (tree_estimation_grammar_alphabet, tree_estimation_chain->class_labels);
	  seq_db.seqs2scores (tree_estimation_hidden_alphabet);

	  // if any trees are missing, estimate them by neighbor-joining
	  if (missing_trees)
	    {
	      Subst_dist_func_factory dist_func_factory (*tree_estimation_chain->matrix);
	      align_db.estimate_missing_trees_by_nj (dist_func_factory);
	    }

	  // optimise branch lengths by EM
	  if (found_matrix && em_max_iter != 0)   // checking em_max_iter here allows user to force neighbor-joining only, by specifying "--maxrounds 0"
	    {
	      Irrev_EM_matrix nj_hsm (1, 1);  // create temporary EM_matrix
	      nj_hsm.assign (*tree_estimation_chain->matrix);
	      align_db.optimise_branch_lengths_by_EM (nj_hsm, 0., em_max_iter, em_forgive, em_min_inc, BRANCH_LENGTH_RES, BRANCH_LENGTH_MAX, min_branch_len);
	    }

	  // clear the (potentially inconsistent with other grammars) Score_profile's
	  seq_db.clear_scores();
	}

      // ensure all tree branches meet minimum length requirement
      for (int n_align = 0; n_align < align_db.size(); n_align++)
	{
	  PHYLIP_tree& tree = align_db.tree_align[n_align]->tree;
	  for_rooted_branches_post (tree, b)
	    if ((*b).length < min_branch_len)
	      tree.branch_length (*b) = min_branch_len;
	}

      // copy all trees back into the Stockholm alignments (ugh)
      for (int n_align = 0; n_align < align_db.size(); n_align++)
	{
	  Tree_alignment& tree_align = *align_db.tree_align[n_align];
	  Stockholm& stock = *stock_db.align_index[n_align];
	  tree_align.copy_tree_to_Stockholm (stock);
	}

      // get list of grammars; either by loading from file, or by using a preset
      Empty_alphabet user_alphabet;
      if (grammars_filename.size())
	{
	  ECFG_builder::load_xgram_alphabet_and_grammars (grammars_filename, user_alphabet, grammar, &align_db, max_subseq_len, tres);
	  alph = &user_alphabet;
	}
      else
	{
	  // initialise grammars from preset
	  ECFG_map::iterator ecfg_iter = ecfg_map.find (preset);
	  if (ecfg_iter == ecfg_map.end())
	    THROWEXPR ("Preset grammar '" << preset << "' not known (did you mean to load a grammar from a file?)");
	  grammar.push_back ((*ecfg_iter).second);

	  // update timepoint_res for loaded preset grammar (hacky)
	  for_const_contents (vector<ECFG_chain>, (*ecfg_iter).second->matrix_set.chain, ec)
	    ec->matrix->timepoint_res = tres;

	  // initialise alphabet from grammar (presets use global singleton alphabet objects)
	  alph = &grammar[0]->alphabet;
	}

      // save macro-expanded grammar (prior to training)
      if (dump_expanded.size())
	{
	  // save
	  ofstream dump_file (dump_expanded.c_str());
	  for (int n = 0; n < (int) grammar.size(); ++n)
	    ECFG_builder::ecfg2stream (dump_file, *alph, *grammar[n], (ECFG_counts*) 0);
	  ECFG_builder::alphabet2stream (dump_file, *alph);
	}

      // create Aligned_score_profile's using the alphabet, for training & annotation
      vector<Aligned_score_profile> asp_vec (align_db.size());
      for (int n = 0; n < align_db.size(); ++n)
	asp_vec[n].init (align_db.tree_align[n]->align, stock_db.align_index[n]->np, *alph);

      // do training
      if (train.size())
	{
	  // train
	  vector<ECFG_trainer*> trainer (grammar.size(), (ECFG_trainer*) 0);
	  if (stock_db.size())
	    for (int n_grammar = 0; n_grammar < (int) grammar.size(); ++n_grammar)
	      {
		ECFG_scores& ecfg = *grammar[n_grammar];
		ECFG_trainer*& tr (trainer[n_grammar]);

		tr = new ECFG_trainer (ecfg, stock_db.align_index, asp_vec, ecfg.is_left_regular() || ecfg.is_right_regular() ? 0 : max_subseq_len);

		tr->pseud_init = pseud_init;
		tr->pseud_mutate = pseud_mutate;
		tr->pseud_wait = pseud_wait;

		tr->em_min_inc = em_min_inc;
		tr->em_max_iter = em_max_iter;

		tr->iterate_quick_EM (em_forgive);
	      }

	  // save
	  ofstream train_file (train.c_str());
	  for (int n = 0; n < (int) grammar.size(); ++n)
	    {
	      // add training meta-info
	      sstring training_meta;
	      training_meta << TRAINING_INFO
			    << " (" << TRAINING_TIME << ' ' << ((unsigned long) time ((time_t*) 0))
			    << ") (" << TRAINING_BITS << ' ' << Nats2Bits (trainer[n]->best_loglike)
			    << ") (" << ALIGN_FILENAME << ' ' << training_alignment_filename
			    << ')';
	      SExpr training_meta_sexpr (training_meta.c_str());
	      grammar[n]->transient_meta.push_back (training_meta_sexpr);
	      // save
	      ECFG_builder::ecfg2stream (train_file, *alph, *grammar[n], stock_db.size() ? &trainer[n]->counts : (ECFG_counts*) 0);
	    }
	  ECFG_builder::alphabet2stream (train_file, *alph);

	  // delete
	  for_const_contents (vector<ECFG_trainer*>, trainer, t)
	    if (*t)
	      delete *t;
	}

      // open GFF file stream
      ofstream* gff_stream = 0;
      if (gff_filename.size())
	{
	  bool has_GFF = false;
	  for_const_contents (vector<ECFG_scores*>, grammar, g)
	    if ((*g)->has_GFF())
	      has_GFF = true;
	  if (!has_GFF)
	    THROWEXPR ("GFF filename specified, but no GFF annotations in grammar");
	  gff_stream = new ofstream (gff_filename.c_str());
	}

      // annotation loop over alignments
      for (int n_align = 0; n_align < align_db.size(); n_align++)
	{
	  // get Tree_alignment
	  const Tree_alignment& tree_align = *align_db.tree_align[n_align];
	  const int seqlen = tree_align.align.columns();

	  // get the original Stockholm alignment
	  Stockholm* stock = stock_db.align_index[n_align];

	  // get the alignment ID, if it has one
	  sstring align_id = stock->get_name();
	  if (!align_id.size())
	    align_id << "Alignment" << n_align+1;

	  // print log message
	  CTAG(7,XFOLD) << "Processing alignment " << align_id
			<< " (" << n_align+1 << " of " << align_db.size()
			<< "): " << seqlen << " columns\n";

	  // get the Aligned_score_profile
	  const Aligned_score_profile& asp = asp_vec[n_align];

	  // clear the #=GF annotation for the Stockholm alignment,
	  // then add the tree as a "#=GF NH" line
	  const sstring nh_tag (Stockholm_New_Hampshire_tag);
	  stock->clear_gf_annot (nh_tag);
	  ostringstream tree_stream;
	  tree_align.tree.write (tree_stream, 0);
	  sstring tree_string (tree_stream.str());
	  tree_string.chomp();
	  stock->add_gf_annot (nh_tag, tree_string);

	  // loop over grammars
	  for (int n_grammar = 0; n_grammar < (int) grammar.size(); ++n_grammar)
	    {
	      ECFG_scores& ecfg = *grammar[n_grammar];
	      const sstring& ecfg_name = ecfg.name;

	      // create fold envelope
	      ECFG_envelope env;
	      env.init (seqlen, ecfg.is_left_regular() || ecfg.is_right_regular() ? 0 : max_subseq_len);

	      // create GFF container
	      GFF_list gff_list;

	      // create null pointer to persistent inside matrix
	      ECFG_inside_matrix* inside_mx = 0;
	      bool delete_inside_mx = false;

	      // create null pointer to persistent inside-outside matrix
	      ECFG_inside_outside_matrix* inout_mx = 0;

	      // create empty CYK traceback & emit_loglike array
	      ECFG_cell_score_map cyk_trace;
	      array2d<Loge> cyk_emit_loglike;
	      bool cyk_emit_loglike_initialized = false;

	      // decide whether we need to do peeling as well as pruning
	      const bool want_hidden_classes = report_hidden_classes && ecfg.has_hidden_classes();
	      const bool want_fill_down = want_hidden_classes || ancrec_CYK_MAP;

	      // do CYK algorithm, if requested
	      bool zero_likelihood = false;  // this flag becomes set if final likelihood is 0, i.e. no traceback path
	      if (annotate)
		{
		  CTAG(6,XFOLD) << "Annotating using grammar '" << ecfg_name << "'\n";

		  // create CYK matrix; get score & traceback; save emit_loglike; & delete matrix
		  ECFG_CYK_matrix* cyk_mx = new ECFG_CYK_matrix (ecfg, *stock, asp, env, !want_fill_down);
		  cyk_mx->fill();
		  const Loge cyk_loglike = cyk_mx->final_loglike;
		  zero_likelihood = (cyk_loglike <= -InfinityLoge);
		  if (zero_likelihood)
		    CLOGERR << "Alignment likelihood P(" << align_id << "|" << ecfg_name << ") is zero; skipping annotation step\n";
		  else
		    cyk_trace = cyk_mx->traceback();

		  cyk_emit_loglike.swap (cyk_mx->emit_loglike);
		  cyk_emit_loglike_initialized = true;

		  delete cyk_mx;

		  // if posterior probabilities requested, or ECFG has hidden classes, do inside-outside
		  if ((report_postprob || report_confidence || want_hidden_classes || ancrec_CYK_MAP) && !zero_likelihood)
		    {
		      inout_mx = new ECFG_inside_outside_matrix (ecfg, *stock, asp, env, (ECFG_counts*) 0);
		      inout_mx->inside.emit_loglike.swap (cyk_emit_loglike);  // re-use emit log-likelihoods calculated by CYK
		      inout_mx->inside.fill_up_flag = false;
		      inout_mx->outside.want_substitution_counts = want_fill_down;  // only call fill_down if necessary
		      inout_mx->fill();

		      zero_likelihood = zero_likelihood || (inout_mx->inside.final_loglike <= -InfinityLoge);

		      if (zero_likelihood)
			CLOGERR << "Alignment likelihood P(" << align_id << "|" << ecfg_name << ") is zero; skipping posterior probability annotation step\n";
		      else
			{
			  if (report_postprob)
			    {
			      sstring pp_tag;
			      pp_tag << LOGPOSTPROB_TAG_PREFIX << '_' << ecfg_name;
			      sstring trace_tag (CYK_STATE_LABEL);
			      inout_mx->annotate_all_post_state_ll (gff_list, align_id, cyk_trace, trace_tag);
			    }

			  if (report_confidence)
			    {
			      sstring pp_tag;
			      pp_tag << CONFIDENCE_TAG_PREFIX << '_' << ecfg_name;
			      inout_mx->annotate (*stock, gff_list, align_id, cyk_trace, pp_tag);
			    }

			  if (want_hidden_classes)
			    inout_mx->annotate_hidden_classes (*stock, cyk_trace);

			  if (ancrec_CYK_MAP)
			    inout_mx->inside.reconstruct_MAP (*stock, cyk_trace, CYK_MAP_reconstruction_tag, ancrec_postprob);
			}
		    }

		  // add score annotation
		  annotate_loglike (*stock, "SC_max", ecfg_name, cyk_loglike);
		}

	      // do Inside algorithm, if requested
	      const bool has_GFF = ecfg.has_GFF();
	      const bool want_inside = report_sumscore || has_GFF;
	      if (want_inside)
		{
		  CTAG(6,XFOLD) << "Running Inside algorithm using grammar '" << ecfg_name << "'\n";

		  // create Inside matrix, or get score from previously created matrix
		  if (inout_mx)
		    inside_mx = &inout_mx->inside;
		  else
		    {
		      inside_mx = new ECFG_inside_matrix (ecfg, *stock, asp, env, true);
		      delete_inside_mx = true;

		      if (cyk_emit_loglike_initialized)  // can we re-use emit log-likelihoods calculated by CYK?
			{
			  inside_mx->emit_loglike.swap (cyk_emit_loglike);
			  inside_mx->fill_up_flag = false;  // clearing this flag tells inside_mx->fill() that inside_mx->emit_loglike is pre-initialized
			}

		      inside_mx->fill();
		    }

		  // TODO:
		  // add stochastic traceback method to ECFG_inside_matrix
		  // create vector<ECFG_cell_score_map> for holding N inside tracebacks
		  // get the tracebacks
		  // do the MAP ancestral reconstructions (rename ancrec_CYK_MAP to ancrec_MAP)

		  // add score annotation
		  if (report_sumscore)
		    annotate_loglike (*stock, "SC_sum", ecfg_name, inside_mx->final_loglike);
		}

	      // annotate CYK traceback *after* DP is finished
	      if (annotate)
		{
		  ecfg.annotate (*stock, cyk_trace);
		  ecfg.make_GFF (gff_list, cyk_trace, align_id.c_str(), inout_mx, inside_mx);
		}

	      // flush GFF
	      if (!gff_list.empty())
		{
		  if (gff_stream)
		    *gff_stream << gff_list;
		  else
		    stock->add_gff (gff_list);
		}

	      // delete matrices
	      if (inout_mx)
		delete inout_mx;
	      if (delete_inside_mx)
		delete inside_mx;
	    }

	  // print out the annotated Stockholm alignment
	  stock->write_Stockholm (cout);

	} // end of annotation loop over alignments

      // close the GFF stream
      if (gff_stream)
	delete gff_stream;
    }
  catch (const Dart_exception& e)
    {
      CLOGERR << "ERROR: " << e.what();
      exit(1);
    }
}
