#include "notebook.h"
#include "config.h"
#include "directories.h"
#include "logging.h"
#include <fstream>
#include <regex>
#include "cmake.h"
#include "filesystem.h"

#if GTKSOURCEVIEWMM_MAJOR_VERSION > 2 & GTKSOURCEVIEWMM_MINOR_VERSION > 17
#include "gtksourceview-3.0/gtksourceview/gtksourcemap.h"
#endif

namespace sigc {
#ifndef SIGC_FUNCTORS_DEDUCE_RESULT_TYPE_WITH_DECLTYPE
  template <typename Functor>
  struct functor_trait<Functor, false> {
    typedef decltype (::sigc::mem_fun(std::declval<Functor&>(),
                                      &Functor::operator())) _intermediate;
    typedef typename _intermediate::result_type result_type;
    typedef Functor functor_type;
  };
#else
  SIGC_FUNCTORS_DEDUCE_RESULT_TYPE_WITH_DECLTYPE
#endif
}

Notebook::TabLabel::TabLabel(const std::string &title) : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL) {
  set_can_focus(false);
  label.set_text(title);
  label.set_can_focus(false);
  button.set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
  button.set_can_focus(false);
  button.set_relief(Gtk::ReliefStyle::RELIEF_NONE);
  //Based on http://www.micahcarrick.com/gtk-notebook-tabs-with-close-button.html
  std::string data =  ".button {\n"
                      "-GtkButton-default-border : 0px;\n"
                      "-GtkButton-default-outside-border : 0px;\n"
                      "-GtkButton-inner-border: 0px;\n"
                      "-GtkWidget-focus-line-width : 0px;\n"
                      "-GtkWidget-focus-padding : 0px;\n"
                      "padding: 0px;\n"
                      "}";
  auto provider = Gtk::CssProvider::create();
  provider->load_from_data(data);
  button.get_style_context()->add_provider(provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  pack_start(label, Gtk::PACK_SHRINK);
  pack_end(button, Gtk::PACK_SHRINK);
  show_all();
}

Notebook::Notebook() : Gtk::Notebook(), last_index(-1) {
  Gsv::init();
  
  signal_switch_page().connect([this](Gtk::Widget* page, guint page_num) {
    last_index=-1;
  });
}

int Notebook::size() {
  return get_n_pages();
}

Source::View* Notebook::get_view(int page) {
  return source_views.at(get_index(page));
}

size_t Notebook::get_index(int page) {
  for(size_t c=0;c<hboxes.size();c++) {
    if(page_num(*hboxes.at(c))==page)
      return c;
  }
  return -1;
}

Source::View* Notebook::get_current_view() {
  return get_view(get_current_page());
}

void Notebook::open(const boost::filesystem::path &file_path) {
  JDEBUG("start");
  for(int c=0;c<size();c++) {
    if(file_path==get_view(c)->file_path) {
      set_current_page(c);
      get_current_view()->grab_focus();
      return;
    }
  }
  
  if(boost::filesystem::exists(file_path)) {
    std::ifstream can_read(file_path.string());
    if(!can_read) {
      Terminal::get().print("Error: could not open "+file_path.string()+"\n", true);
      return;
    }
    can_read.close();
  }
  
  auto language=Source::guess_language(file_path);
  boost::filesystem::path project_path;
  auto &directories=Directories::get();
  if(directories.cmake && directories.cmake->project_path!="" && file_path.generic_string().substr(0, directories.cmake->project_path.generic_string().size()+1)==directories.cmake->project_path.generic_string()+'/')
    project_path=directories.cmake->project_path;
  else {
    project_path=file_path.parent_path();
    CMake cmake(project_path);
    if(cmake.project_path!="") {
      project_path=cmake.project_path;
      Terminal::get().print("Project path for "+file_path.string()+" set to "+project_path.string()+"\n");
    }
  }
  if(language && (language->get_id()=="chdr" || language->get_id()=="cpphdr" || language->get_id()=="c" || language->get_id()=="cpp" || language->get_id()=="objc")) {
    CMake::create_default_build(project_path);
    source_views.emplace_back(new Source::ClangView(file_path, project_path, language));
  }
  else
    source_views.emplace_back(new Source::GenericView(file_path, project_path, language));
  
  source_views.back()->on_update_status=[this](Source::View* view, const std::string &status_text) {
    if(get_current_page()!=-1 && get_current_view()==view)
      status.set_text(status_text+" ");
  };
  source_views.back()->on_update_info=[this](Source::View* view, const std::string &info_text) {
    if(get_current_page()!=-1 && get_current_view()==view)
      info.set_text(" "+info_text);
  };
  
  scrolled_windows.emplace_back(new Gtk::ScrolledWindow());
  hboxes.emplace_back(new Gtk::HBox());
  scrolled_windows.back()->add(*source_views.back());
  hboxes.back()->pack_start(*scrolled_windows.back());

#if GTKSOURCEVIEWMM_MAJOR_VERSION > 2 & GTKSOURCEVIEWMM_MINOR_VERSION > 17
  source_maps.emplace_back(Glib::wrap(gtk_source_map_new()));
  gtk_source_map_set_view(GTK_SOURCE_MAP(source_maps.back()->gobj()), source_views.back()->gobj());
#endif
  configure(source_views.size()-1);
  
  //Set up tab label
  std::string title=file_path.filename().string();
  tab_labels.emplace_back(new TabLabel(title));
  auto source_view=source_views.back();
  tab_labels.back()->button.signal_clicked().connect([this, source_view](){
    for(int c=0;c<size();c++) {
      if(get_view(c)==source_view) {
        close(c);
        break;
      }
    }
  });
  append_page(*hboxes.back(), *tab_labels.back());
  
  set_tab_reorderable(*hboxes.back(), true);
  show_all_children();
  
  size_t last_index_tmp=-1;
  if(get_current_page()!=-1)
    last_index_tmp=get_index(get_current_page());
  set_current_page(size()-1);
  last_index=last_index_tmp;
  
  set_focus_child(*source_views.back());
  get_current_view()->get_buffer()->set_modified(false);
  get_current_view()->grab_focus();
  //Add star on tab label when the page is not saved:
  get_current_view()->get_buffer()->signal_modified_changed().connect([this, source_view]() {
    std::string title=source_view->file_path.filename().string();
    if(source_view->get_buffer()->get_modified())
      title+="*";
    int page=-1;
    for(int c=0;c<size();c++) {
      if(get_view(c)==source_view) {
        page=c;
        break;
      }
    }
    if(page!=-1)
      tab_labels.at(get_index(page))->label.set_text(title);
  });
  
  JDEBUG("end");
}

void Notebook::configure(int view_nr) {
#if GTKSOURCEVIEWMM_MAJOR_VERSION > 2 & GTKSOURCEVIEWMM_MINOR_VERSION > 17
  auto source_font_description=Pango::FontDescription(Config::get().source.font);
  auto source_map_font_desc=Pango::FontDescription(static_cast<std::string>(source_font_description.get_family())+" "+Config::get().source.map_font_size); 
  source_maps.at(view_nr)->override_font(source_map_font_desc);
  if(Config::get().source.show_map) {
    if(hboxes.at(view_nr)->get_children().size()==1)
      hboxes.at(view_nr)->pack_end(*source_maps.at(view_nr), Gtk::PACK_SHRINK);
  }
  else if(hboxes.at(view_nr)->get_children().size()==2)
    hboxes.at(view_nr)->remove(*source_maps.at(view_nr));
#endif
}

bool Notebook::save(int page) {
  JDEBUG("start");
  if(page>=size()) {
    JDEBUG("end false");
    return false;
  }
  auto view=get_view(page);
  if (view->file_path != "" && view->get_buffer()->get_modified()) {
    //Remove trailing whitespace characters on save, and add trailing newline if missing
    if(Config::get().source.cleanup_whitespace_characters) {
      auto buffer=view->get_buffer();
      buffer->begin_user_action();
      for(int line=0;line<buffer->get_line_count();line++) {
        auto iter=buffer->get_iter_at_line(line);
        auto end_iter=iter;
        while(!end_iter.ends_line())
          end_iter.forward_char();
        if(iter==end_iter)
          continue;
        iter=end_iter;
        while(!iter.starts_line() && (*iter==' ' || *iter=='\t' || iter.ends_line()))
          iter.backward_char();
        if(*iter!=' ' && *iter!='\t')
          iter.forward_char();
        if(iter==end_iter)
          continue;
        buffer->erase(iter, end_iter);
      }
      auto iter=buffer->end();
      if(!iter.starts_line())
        buffer->insert(buffer->end(), "\n");
      buffer->end_user_action();
    }
    
    if(filesystem::write(view->file_path, view->get_buffer())) {
      if(auto clang_view=dynamic_cast<Source::ClangView*>(view)) {
        if(clang_view->language->get_id()=="chdr" || clang_view->language->get_id()=="cpphdr") {
          for(auto a_view: source_views) {
            if(auto a_clang_view=dynamic_cast<Source::ClangView*>(a_view)) {
                if(clang_view!=a_clang_view)
                  a_clang_view->soft_reparse_needed=true;
            }
          }
        }
      }
      
      view->get_buffer()->set_modified(false);
      
      //If CMakeLists.txt have been modified:
      boost::filesystem::path project_path;
      if(view->file_path.filename()=="CMakeLists.txt") {
        auto &directories=Directories::get();
        if(directories.cmake && directories.cmake->project_path!="" && view->file_path.generic_string().substr(0, directories.cmake->project_path.generic_string().size()+1)==directories.cmake->project_path.generic_string()+'/') {
          if(CMake::create_default_build(directories.cmake->project_path, true))
            project_path=directories.cmake->project_path;
        }
        else {
          CMake cmake(view->file_path.parent_path());
          if(CMake::create_default_build(cmake.project_path, true))
            project_path=cmake.project_path;
        }
        if(project_path!="") {
          auto debug_project_path=CMake::get_debug_build_path(project_path);
          if(!debug_project_path.empty() && boost::filesystem::exists(debug_project_path))
            CMake::create_debug_build(project_path);
          for(auto source_view: source_views) {
            if(auto source_clang_view=dynamic_cast<Source::ClangView*>(source_view)) {
              if(project_path==source_clang_view->project_path)
                source_clang_view->full_reparse_needed=true;
            }
          }
        }
      }
      JDEBUG("end true");
      return true;
    }
    Terminal::get().print("Error: could not save file " +view->file_path.string()+"\n", true);
  }
  JDEBUG("end false");
  return false;
}

bool Notebook::save_current() {
  if(get_current_page()==-1)
    return false;
  return save(get_current_page());
}

void Notebook::save_project_files() {
  if(get_current_page()==-1)
    return;
  auto current_view=get_current_view();
  for(int c=0;c<size();c++) {
    auto view=get_view(c);
    if(view->get_buffer()->get_modified()) {
      if(current_view->project_path==view->project_path)
        save(c);
    }
  }
}

bool Notebook::close(int page) {
  JDEBUG("start");
  if (page!=-1) {
    auto view=get_view(page);
    if(view->get_buffer()->get_modified()){
      if(!save_modified_dialog(page)) {
        JDEBUG("end false");
        return false;
      }
    }
    auto index=get_index(page);
    if(page==get_current_page()) {
      if(last_index!=static_cast<size_t>(-1)) {
        set_current_page(page_num(*hboxes.at(last_index)));
        last_index=-1;
      }
    }
    else if(index==last_index)
      last_index=-1;
    else if(index<last_index && last_index!=static_cast<size_t>(-1))
      last_index--;
    
    remove_page(page);
#if GTKSOURCEVIEWMM_MAJOR_VERSION > 2 & GTKSOURCEVIEWMM_MINOR_VERSION > 17
    source_maps.erase(source_maps.begin()+index);
#endif

    auto source_view=source_views.at(index);
    if(auto source_clang_view=dynamic_cast<Source::ClangView*>(source_view))
      source_clang_view->async_delete();
    else
      delete source_view;
    source_views.erase(source_views.begin()+index);
    scrolled_windows.erase(scrolled_windows.begin()+index);
    hboxes.erase(hboxes.begin()+index);
    tab_labels.erase(tab_labels.begin()+index);
  }
  JDEBUG("end true");
  return true;
}

bool Notebook::close_current_page() {
  if(get_current_page()==-1)
    return false;
  return close(get_current_page());
}

boost::filesystem::path Notebook::get_current_folder() {
  boost::filesystem::path current_path;
  
  if(get_current_page()!=-1)
    current_path=get_current_view()->project_path;
  else
    current_path=Directories::get().current_path;
  
  return current_path;
}

bool Notebook::save_modified_dialog(int page) {
  Gtk::MessageDialog dialog((Gtk::Window&)(*get_toplevel()), "Save file!", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
  dialog.set_default_response(Gtk::RESPONSE_YES);
  dialog.set_secondary_text("Do you want to save: " + get_view(page)->file_path.string()+" ?");
  int result = dialog.run();
  if(result==Gtk::RESPONSE_YES) {
    save(page);
    return true;
  }
  else if(result==Gtk::RESPONSE_NO) {
    return true;
  }
  else {
    return false;
  }
}

