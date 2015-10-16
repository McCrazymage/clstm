#include "pstring.h"
#include "clstm.h"
#include "clstmhl.h"
#include <assert.h>
#include <iostream>
#include <vector>
#include <memory>
#include <math.h>
#include <Eigen/Dense>
#include <sstream>
#include <fstream>
#include <iostream>
#include <set>
#include <regex>
#include <fenv.h>
#include "multidim.h"
#include "pymulti.h"
#include "extras.h"

using namespace Eigen;
using namespace ocropus;
using namespace pymulti;
using std::vector;
using std::map;
using std::make_pair;
using std::shared_ptr;
using std::unique_ptr;
using std::cout;
using std::ifstream;
using std::set;
using std::to_string;
using std_string = std::string;
using std_wstring = std::wstring;
using std::regex;
using std::regex_replace;
#define string std_string
#define wstring std_wstring

string basename(string s) {
  int start = 0;
  for (;;) {
    auto pos = s.find("/", start);
    if (pos == string::npos) break;
    start = pos + 1;
  }
  auto pos = s.find(".", start);
  if (pos == string::npos)
    return s;
  else
    return s.substr(0, pos);
}

string read_text(string fname, int maxsize = 65536) {
  char buf[maxsize];
  buf[maxsize - 1] = 0;
  ifstream stream(fname);
  stream.read(buf, maxsize - 1);
  int n = stream.gcount();
  while (n > 0 && buf[n - 1] == '\n') n--;
  return string(buf, n);
}

wstring read_text32(string fname, int maxsize = 65536) {
  char buf[maxsize];
  buf[maxsize - 1] = 0;
  ifstream stream(fname);
  stream.read(buf, maxsize - 1);
  int n = stream.gcount();
  while (n > 0 && buf[n - 1] == '\n') n--;
  return utf8_to_utf32(string(buf, n));
}

void show(PyServer &py, Sequence &s, int subplot = 0) {
  int n = s.size();
  int m = s.rows();
  mdarray<float> temp;
  temp.resize(n,m);
  for(int i=0;i<n;i++)
    for(int j=0;j<m;j++)
      temp(i,j) = s[i].v(j,0);
  if (subplot > 0) py.evalf("subplot(%d)", subplot);
  py.imshowT(temp, "cmap=cm.hot");
}

void read_lines(vector<string> &lines, string fname) {
  ifstream stream(fname);
  string line;
  lines.clear();
  while (getline(stream, line)) {
    lines.push_back(line);
  }
}

wstring separate_chars(const wstring &s, const wstring &charsep) {
  if (charsep == L"") return s;
  wstring result;
  for (int i = 0; i < s.size(); i++) {
    if (i > 0) result.push_back(charsep[0]);
    result.push_back(s[i]);
  }
  return result;
}

int main1(int argc, char **argv) {
  if(getienv("fpe",1)) feenableexcept(FE_ALL_EXCEPT & ~FE_INEXACT);
  srandomize();

  int ntrain = getienv("ntrain", 10000000);
  int save_every = getienv("save_every", 10000);
  string save_name = getsenv("save_name", "_ocr");
  int report_every = getienv("report_every", 100);
  int display_every = getienv("display_every", 0);
  int report_time = getienv("report_time", 0);
  int test_every = getienv("test_every", 10000);
  wstring charsep = utf8_to_utf32(getsenv("charsep", ""));

  if (argc < 2 || argc > 3) THROW("... training [testing]");
  vector<string> fnames, test_fnames;
  read_lines(fnames, argv[1]);
  if (argc > 2) read_lines(test_fnames, argv[2]);
  print("got", fnames.size(), "files,", test_fnames.size(), "tests");

  string load_name = getsenv("load", "");

  CLSTMOCR clstm;

  if (load_name != "") {
    clstm.load(load_name);
  } else {
    Codec codec;
    vector<string> gtnames;
    for (auto s : fnames) gtnames.push_back(basename(s) + ".gt.txt");
    codec.build(gtnames, charsep);
    print("got", codec.size(), "classes");

    clstm.target_height = int(getrenv("target_height", 48));
    clstm.createBidi(codec.codec, getienv("nhidden", 100));
    clstm.setLearningRate(getdenv("lrate", 1e-4), getdenv("momentum", 0.9));
  }
  network_info(clstm.net);

  double test_error = 9999.0;
  double best_error = 1e38;

  PyServer py;
  if (display_every > 0) py.open();
  double start_time = now();
  int start = clstm.net->attr.get("trial", getienv("start", -1)) + 1;
  if (start > 0) print("start", start);
  for (int trial = start; trial < ntrain; trial++) {
    if (trial > 0 && test_fnames.size() > 0 && test_every > 0 &&
        trial % test_every == 0) {
      double errors = 0.0;
      double count = 0.0;
      for (int test = 0; test < test_fnames.size(); test++) {
        string fname = test_fnames[test];
        string base = basename(fname);
        wstring gt = separate_chars(read_text32(base + ".gt.txt"), charsep);
        mdarray<float> raw;
        read_png(raw, fname.c_str(), true);
        for (int i = 0; i < raw.size(); i++) raw[i] = 1 - raw[i];
        wstring pred = clstm.predict(raw);
        count += gt.size();
        errors += levenshtein(pred, gt);
      }
      test_error = errors / count;
      print("ERROR", trial, test_error, "   ", errors, count);
    }
    if (save_every == 0 && test_error < best_error) {
      best_error = test_error;
      string fname = save_name + ".clstm";
      print("saving best performing network so far", fname, "error rate: ",
            best_error);
      clstm.net->attr.set("trial", trial);
      clstm.save(fname);
    }
    bool do_save = (save_every > 0 && trial % save_every == 0);
    do_save = (do_save || (trial == ntrain - 1));
    do_save = (do_save && (save_name != ""));
    if (trial > 0 && do_save) {
      string fname = save_name + "-" + to_string(trial) + ".clstm";
      clstm.net->attr.set("trial", trial);
      clstm.save(fname);
    }
    int sample = irandom() % fnames.size();
    string fname = fnames[sample];
    string base = basename(fname);
    wstring gt = separate_chars(read_text32(base + ".gt.txt"), charsep);
    mdarray<float> raw;
    read_png(raw, fname.c_str(), true);
    for (int i = 0; i < raw.size(); i++) raw[i] = 1 - raw[i];
    wstring pred = clstm.train(raw, gt);
    if (display_every > 0 && trial % display_every == 0) {
      py.evalf("clf");
      show(py, clstm.net->inputs, 411);
      show(py, clstm.net->outputs, 412);
      show(py, clstm.targets, 413);
      show(py, clstm.aligned, 414);
    }
    if (report_every > 0 && trial % report_every == 0) {
      mdarray<float> temp;
      print(trial);
      print("TRU", gt);
      print("ALN", clstm.aligned_utf8());
      print("OUT", utf32_to_utf8(pred));
      if (trial > 0 && report_time)
        print("steptime", (now() - start_time) / report_every);
      start_time = now();
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  TRY { return main1(argc, argv); }
  CATCH(const char *message) { cerr << "FATAL: " << message << endl; }
}
