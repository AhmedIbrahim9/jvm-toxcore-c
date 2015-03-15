open ApiAst


type 'a t = {
  fold_uname : 'a t -> 'a -> uname -> 'a * uname;
  fold_lname : 'a t -> 'a -> lname -> 'a * lname;
  fold_macro : 'a t -> 'a -> macro -> 'a * macro;
  fold_comment_fragment : 'a t -> 'a -> comment_fragment -> 'a * comment_fragment;
  fold_comment : 'a t -> 'a -> comment -> 'a * comment;
  fold_size_spec : 'a t -> 'a -> size_spec -> 'a * size_spec;
  fold_type_name : 'a t -> 'a -> type_name -> 'a * type_name;
  fold_enumerator : 'a t -> 'a -> enumerator -> 'a * enumerator;
  fold_error_list : 'a t -> 'a -> error_list -> 'a * error_list;
  fold_parameter : 'a t -> 'a -> parameter -> 'a * parameter;
  fold_function_name : 'a t -> 'a -> function_name -> 'a * function_name;
  fold_expr : 'a t -> 'a -> expr -> 'a * expr;
  fold_decl : 'a t -> 'a -> decl -> 'a * decl;
}


let fold_list f v state l =
  let state, l =
    List.fold_left
      (fun (state, l) elt ->
         let state, elt = f v state elt in
         state, elt :: l
      ) (state, []) l
  in
  state, List.rev l


let fold_uname v state = function
  | UName (id, name) -> state, UName (id, name)

let fold_lname v state = function
  | LName (id, name) -> state, LName (id, name)

let fold_macro v state = function
  | Macro macro -> state, Macro macro


let fold_comment_fragment v state = function
  | Cmtf_Doc doc ->
      state, Cmtf_Doc doc
  | Cmtf_UName uname ->
      let state, uname = v.fold_uname v state uname in
      state, Cmtf_UName uname
  | Cmtf_LName lname ->
      let state, lname = v.fold_lname v state lname in
      state, Cmtf_LName lname
  | Cmtf_Break ->
      state, Cmtf_Break


let fold_comment v state = function
  | Cmt_None ->
      state, Cmt_None
  | Cmt_Doc frags ->
      let state, frags = fold_list v.fold_comment_fragment v state frags in
      state, Cmt_Doc frags
  | Cmt_Section frags ->
      let state, frags = fold_list v.fold_comment_fragment v state frags in
      state, Cmt_Section frags


let rec fold_size_spec v state = function
  | Ss_UName uname ->
      let state, uname = v.fold_uname v state uname in
      state, Ss_UName uname
  | Ss_LName lname ->
      let state, lname = v.fold_lname v state lname in
      state, Ss_LName lname
  | Ss_Size ->
      state, Ss_Size
  | Ss_Bounded (size_spec, uname) ->
      let state, size_spec = v.fold_size_spec v state size_spec in
      let state, uname = v.fold_uname v state uname in
      state, Ss_Bounded (size_spec, uname)


let rec fold_type_name v state = function
  | Ty_UName uname ->
      let state, uname = v.fold_uname v state uname in
      state, Ty_UName uname
  | Ty_LName lname ->
      let state, lname = v.fold_lname v state lname in
      state, Ty_LName lname
  | Ty_Array (lname, size_spec) ->
      let state, lname = v.fold_lname v state lname in
      let state, size_spec = v.fold_size_spec v state size_spec in
      state, Ty_Array (lname, size_spec)
  | Ty_This ->
      state, Ty_This
  | Ty_Const type_name ->
      let state, type_name = v.fold_type_name v state type_name in
      state, Ty_Const type_name


let rec fold_enumerator v state = function
  | Enum_Name (comment, uname) ->
      let state, comment = v.fold_comment v state comment in
      let state, uname = v.fold_uname v state uname in
      state, Enum_Name (comment, uname)
  | Enum_Namespace (uname, enumerators) ->
      let state, uname = v.fold_uname v state uname in
      let state, enumerators = fold_list v.fold_enumerator v state enumerators in
      state, Enum_Namespace (uname, enumerators)


let fold_error_list v state = function
  | Err_None ->
      state, Err_None
  | Err_From lname ->
      let state, lname = v.fold_lname v state lname in
      state, Err_From lname
  | Err_List enumerators ->
      let state, enumerators = fold_list v.fold_enumerator v state enumerators in
      state, Err_List enumerators


let fold_parameter v state = function
  | Param (type_name, lname) ->
      let state, type_name = v.fold_type_name v state type_name in
      let state, lname = v.fold_lname v state lname in
      state, Param (type_name, lname)


let fold_function_name v state = function
  | Fn_Custom (type_name, lname) ->
      let state, type_name = v.fold_type_name v state type_name in
      let state, lname = v.fold_lname v state lname in
      state, Fn_Custom (type_name, lname)
  | Fn_Size -> state, Fn_Size
  | Fn_Get -> state, Fn_Get
  | Fn_Set -> state, Fn_Set


let rec fold_expr v state = function
  | E_Number num ->
      state, E_Number num
  | E_UName uname ->
      let state, uname = v.fold_uname v state uname in
      state, E_UName uname
  | E_Sizeof lname ->
      let state, lname = v.fold_lname v state lname in
      state, E_Sizeof lname
  | E_Plus (lhs, rhs) ->
      let state, lhs = v.fold_expr v state lhs in
      let state, rhs = v.fold_expr v state rhs in
      state, E_Plus (lhs, rhs)


let rec fold_decl v state = function
  | Decl_Comment (comment, decl) ->
      let state, comment = v.fold_comment v state comment in
      let state, decl = v.fold_decl v state decl in
      state, Decl_Comment (comment, decl)
  | Decl_Static decl ->
      let state, decl = v.fold_decl v state decl in
      state, Decl_Static decl
  | Decl_Macro macro ->
      let state, macro = v.fold_macro v state macro in
      state, Decl_Macro macro
  | Decl_Namespace (lname, decls) ->
      let state, lname = v.fold_lname v state lname in
      let state, decls = fold_list v.fold_decl v state decls in
      state, Decl_Namespace (lname, decls)
  | Decl_Class (lname, decls) ->
      let state, lname = v.fold_lname v state lname in
      let state, decls = fold_list v.fold_decl v state decls in
      state, Decl_Class (lname, decls)
  | Decl_Function (function_name, parameters, error_list) ->
      let state, function_name = v.fold_function_name v state function_name in
      let state, parameters = fold_list v.fold_parameter v state parameters in
      let state, error_list = v.fold_error_list v state error_list in
      state, Decl_Function (function_name, parameters, error_list)
  | Decl_Const (uname, expr) ->
      let state, uname = v.fold_uname v state uname in
      let state, expr = v.fold_expr v state expr in
      state, Decl_Const (uname, expr)
  | Decl_Enum (is_class, uname, enumerators) ->
      let state, uname = v.fold_uname v state uname in
      let state, enumerators = fold_list v.fold_enumerator v state enumerators in
      state, Decl_Enum (is_class, uname, enumerators)
  | Decl_Error (lname, enumerators) ->
      let state, lname = v.fold_lname v state lname in
      let state, enumerators = fold_list v.fold_enumerator v state enumerators in
      state, Decl_Error (lname, enumerators)
  | Decl_Struct decls ->
      let state, decls = fold_list v.fold_decl v state decls in
      state, Decl_Struct decls
  | Decl_Member (type_name, lname) ->
      let state, type_name = v.fold_type_name v state type_name in
      let state, lname = v.fold_lname v state lname in
      state, Decl_Member (type_name, lname)
  | Decl_GetSet (type_name, lname, decls) ->
      let state, type_name = v.fold_type_name v state type_name in
      let state, lname = v.fold_lname v state lname in
      let state, decls = fold_list v.fold_decl v state decls in
      state, Decl_GetSet (type_name, lname, decls)
  | Decl_Typedef (lname, parameters) ->
      let state, lname = v.fold_lname v state lname in
      let state, parameters = fold_list v.fold_parameter v state parameters in
      state, Decl_Typedef (lname, parameters)
  | Decl_Event (lname, decl) ->
      let state, lname = v.fold_lname v state lname in
      let state, decl = v.fold_decl v state decl in
      state, Decl_Event (lname, decl)


let fold_decls v state = fold_list v.fold_decl v state


let default = {
  fold_uname;
  fold_lname;
  fold_macro;
  fold_comment_fragment;
  fold_comment;
  fold_size_spec;
  fold_type_name;
  fold_enumerator;
  fold_error_list;
  fold_parameter;
  fold_function_name;
  fold_expr;
  fold_decl;
}
